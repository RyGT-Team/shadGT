#include "video_core/renderer_vulkan/compile_cpu_batch.h"
#include "video_core/renderer_vulkan/fragment_descriptor_policy.h"
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <map>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <boost/container/small_vector.hpp>

#include "common/assert.h"
#include "common/elf_info.h"
#include "common/singleton.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/perf_monitor.h"
#include "common/thread.h"
#include "shader_recompiler/backend/spirv/emit_spirv_discard_frag.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "video_core/renderer_vulkan/compile_pack.h"
#include "video_core/renderer_vulkan/compile_pack_pipeline.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/shared_build_map.h"
#include "video_core/renderer_vulkan/shader_object_cache.h"
#include "video_core/renderer_vulkan/shader_object_policy.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/compiler_worker_budget.h"
#include "video_core/renderer_vulkan/idle_compile_queue.h"
#include "video_core/renderer_vulkan/shared_pipeline_policy.h"
#include "video_core/renderer_vulkan/fragment_multisample_key.h"
#include "video_core/renderer_vulkan/shared_creation_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_binary_store.h" // BIN-001
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace Vulkan {
std::counting_semaphore<64>& HeavyPipelineGate() {
    static std::counting_semaphore<64> semaphore{CompilerWorkerBudget::HeavyPipelineLimit};
    return semaphore;
}
#ifdef RYTY_GPU
bool NativeModeRequested();
#endif

#ifdef _WIN32
// DIAG-100: every thread's stack to user\log\<file_name> (tools\stackdump.exe next to
// GTSport.exe), for a build or the GPU stuck for long.
void DumpThreadStacks(const wchar_t* file_name) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::filesystem::path dir = std::filesystem::path{exe}.parent_path();
    const auto tool = dir / "tools" / "stackdump.exe";
    if (!std::filesystem::exists(tool)) {
        return;
    }
    const auto out = dir / "user" / "log" / file_name;
    std::wstring command =
        L"\"" + tool.wstring() + L"\" " + std::to_wstring(GetCurrentProcessId()) + L" 512";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE file = CreateFileW(out.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = file;
    si.hStdError = file;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LOG_WARNING(Render_Vulkan, "DIAG-100: thread stacks written to {}", out.string());
    }
    CloseHandle(file);
}
#else
void DumpThreadStacks(const wchar_t*) {}
#endif

// FIX-104: pipeline creations in progress (frame generation pauses meanwhile).
static std::atomic<u32> g_builds_running{0};
u32 PipelineBuildsRunning() {
    return g_builds_running.load(std::memory_order_relaxed);
}

namespace {

// DIAG-049: graphics pipeline creations the driver takes long on, logged every 2 s while they
// run with the memory the process has committed. On GT Sport's race summary the driver's
// compiler used up all memory ("LLVM ERROR: out of memory").
class BuildWatch {
public:
    BuildWatch(std::string name_, const char* what_) : name{std::move(name_)}, what{what_} {
        g_builds_running.fetch_add(1, std::memory_order_relaxed); // FIX-104
        auto& state = State();
        std::scoped_lock lk{state.mutex};
        id = ++state.next_id;
        state.running.emplace(id, Entry{name, what, std::chrono::steady_clock::now(), false, 0});
        if (!state.watcher_started) {
            state.watcher_started = true;
            std::thread{Watch}.detach();
        }
    }
    ~BuildWatch() {
        g_builds_running.fetch_sub(1, std::memory_order_relaxed); // FIX-104
        auto& state = State();
        std::scoped_lock lk{state.mutex};
        const auto it = state.running.find(id);
        if (it->second.reported) {
            LOG_WARNING(Render_Vulkan, "DIAG-049: {} of {} done after {:.1f} s; {} MB committed",
                        what, name, Seconds(it->second.start), CommittedMb());
        }
        state.running.erase(it);
    }

private:
    struct Entry {
        std::string name;
        const char* what;
        std::chrono::steady_clock::time_point start;
        bool reported;
        double last_report = 0;
    };
    struct Shared {
        std::mutex mutex;
        std::unordered_map<u64, Entry> running;
        u64 next_id = 0;
        bool watcher_started = false;
    };
    static Shared& State() {
        static Shared state;
        return state;
    }
    static double Seconds(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    static u64 CommittedMb() {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX counters{};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                    sizeof(counters))) {
            return counters.PrivateUsage >> 20;
        }
#endif
        return 0;
    }
    static void Watch() {
        Common::SetCurrentThreadName("shadGT:BuildWatch");
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds{2});
            auto& state = State();
            std::scoped_lock lk{state.mutex};
            u32 long_running = 0;
            for (auto& [id, entry] : state.running) {
                const double seconds = Seconds(entry.start);
                long_running += seconds >= 3.0;
                if (seconds >= 3.0 && (!entry.reported || seconds - entry.last_report >= 10.0)) {
                    entry.reported = true;
                    entry.last_report = seconds;
                    LOG_WARNING(Render_Vulkan,
                                "DIAG-049: {} of {} running for {:.1f} s; {} MB committed",
                                entry.what, entry.name, Seconds(entry.start), CommittedMb());
                }
            }
            if (long_running != 0) {
                LOG_WARNING(Render_Vulkan, "DIAG-049: {} pipeline creations of {} running for 3 s "
                            "or more; {} MB committed", long_running, state.running.size(),
                            CommittedMb());
            }
#ifdef _WIN32
            // DIAG-100: a creation stuck for 30 s: once per run, every thread's stack goes to
            // user\log\stuck-stacks.txt (tools\stackdump.exe next to GTSport.exe).
            static bool dumped = false;
            const bool stuck = std::ranges::any_of(
                state.running, [](const auto& entry) { return Seconds(entry.second.start) >= 30.0; });
            if (stuck && !dumped) {
                dumped = true;
                DumpThreadStacks(L"stuck-stacks.txt");
            }
#endif
        }
    }

    std::string name;
    const char* what;
    u64 id{};
};

// FIX-057: the driver compiles a tessellated pipeline with a lot of memory, and GT Sport's race
// summary (the driver figure) needs hundreds of new ones at once. With every build worker on one,
// the process went from 11 GB to 100 GB in seconds and the driver's compiler ran out of memory.
// Admit one heavy compile at a time: two simultaneous builds exhausted system
// commit inside NVIDIA LLVM. -DisablePerf 79 disables this final driver-call gate.
// PERF-172 (GT7): RYGT_TESS_BUILDS=N (2..16, default 4 since PERF-419-GTS as in shadgt7) lets
// up to N tessellated pipelines build at once while the process commits under half the RAM and
// 4 GB stay free, two otherwise (GT7's unoptimized tessellated pipelines, FIX-057/PERF-081, held
// its cars back two at a time). RYGT_TESS_BUILDS=1: the shared one-at-a-time gate above applies.
class HeavyBuildGate {
public:
    explicit HeavyBuildGate(bool heavy) {
        static const bool enabled = Common::PerfFeatureEnabled(79);
        if (!heavy || !enabled) {
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        if (Asked() == 0) {
            Semaphore().acquire();
            semaphore = true;
        } else {
            auto& gate = State();
            std::unique_lock lk{gate.mutex};
            gate.cv.wait(lk, [&] { return gate.running < Limit(); });
            ++gate.running;
        }
        held = true;
        const auto waited = std::chrono::steady_clock::now() - start;
        if (waited >= std::chrono::seconds{1}) {
            static std::atomic<u32> logged{};
            if (const u32 n = ++logged; n <= 20 || n % 100 == 0) {
                LOG_WARNING(Render_Vulkan,
                            "FIX-057: a tessellated pipeline waited {:.1f} s for its turn ({} "
                            "times)",
                            std::chrono::duration<double>(waited).count(), n);
            }
        }
    }
    ~HeavyBuildGate() {
        if (!held) {
            return;
        }
        if (semaphore) {
            Semaphore().release();
            return;
        }
        auto& gate = State();
        {
            std::scoped_lock lk{gate.mutex};
            --gate.running;
        }
        gate.cv.notify_all();
    }

private:
    static std::counting_semaphore<64>& Semaphore() {
        return HeavyPipelineGate();
    }
    struct Gate {
        std::mutex mutex;
        std::condition_variable cv;
        u32 running = 0;
    };
    static Gate& State() {
        static Gate gate;
        return gate;
    }
    /// PERF-172: RYGT_TESS_BUILDS (default 4), or 0 when it is 1 (the shared semaphore then).
    static u32 Asked() {
        const u32 n = TessBuildsAsked();
        return n > 1 ? n : 0u;
    }
    static u32 Limit() {
        const u32 asked = Asked();
        if (asked <= 2) {
            return asked;
        }
#ifdef _WIN32
        MEMORYSTATUSEX status{.dwLength = sizeof(MEMORYSTATUSEX)};
        PROCESS_MEMORY_COUNTERS_EX counters{};
        if (GlobalMemoryStatusEx(&status) &&
            K32GetProcessMemoryInfo(GetCurrentProcess(),
                                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                    sizeof(counters))) {
            const bool tight = counters.PrivateUsage > status.ullTotalPhys / 2 ||
                               status.ullAvailPhys < (4ull << 30);
            if (tight) {
                static std::atomic<bool> logged{};
                if (!logged.exchange(true)) {
                    LOG_WARNING(Render_Vulkan,
                                "PERF-172: memory is tight ({} MB committed, {} MB free); "
                                "tessellated pipelines build two at a time",
                                counters.PrivateUsage >> 20, status.ullAvailPhys >> 20);
                }
                return 2;
            }
        }
#endif
        return asked;
    }
    bool held = false;
    bool semaphore = false;
};

} // namespace

using Shader::Backend::SPIRV::AuxShaderType;

static constexpr std::array LogicalStageToStageBit = {
    vk::ShaderStageFlagBits::eFragment,
    vk::ShaderStageFlagBits::eTessellationControl,
    vk::ShaderStageFlagBits::eTessellationEvaluation,
    vk::ShaderStageFlagBits::eVertex,
    vk::ShaderStageFlagBits::eGeometry,
    vk::ShaderStageFlagBits::eCompute,
};

// PERF-021: time of the last pipeline a draw needed that did not exist yet.
static std::atomic<s64> last_pipeline_miss_ms{};

// Preserve the existing ordinary/UI policy: optional optimized replacement is opt-in.
// Tessellation never retains optimizer IR or enters LTO (previous multi-GB failures).
// AMD-PERF-001 was reverted: RyGTS AMD-322 found that on an RX 7600 link-time data and the
// optimized link made menu pipelines take 0.4-0.7 s and filled pipeline memory (4.4 GB), and
// here it made loading much slower and ran out of memory. RYGT_PIPELINE_LTO=1 still trials it.
static bool LinkTimeOptimization() {
    static const bool lto = [] {
        const char* env = std::getenv("RYGT_PIPELINE_LTO");
        return env && env[0] == '1';
    }();
    return lto;
}
static std::atomic<u64> lto_links{};
static std::atomic<u64> lto_us{};

static s64 SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void GraphicsPipeline::NotePipelineMiss() {
    last_pipeline_miss_ms.store(SteadyMs(), std::memory_order_relaxed);
}

// PERF-017: workers that link fully optimized pipelines from stage libraries, so the command
// thread only waits for the fast link.
class PipelineLinkWorkers {
public:
    static PipelineLinkWorkers& Instance() { static PipelineLinkWorkers workers; return workers; }
    bool TryPush(IdleCompileQueue::Job job) { return queue.TryPush(std::move(job)); }
private:
    IdleCompileQueue queue{32, [] {
        Common::SetCurrentThreadName("shadGT:PipelineLink");
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        SetThreadPriorityBoost(GetCurrentThread(), TRUE);
#endif
    }};
};

// PERF-018: shader stage libraries (pre-rasterization and fragment shader) of runtime
// pipelines, shared by every pipeline whose stages, layout and stage state are the same. Many new
// pipelines differ from an earlier one only in blending, targets or vertex input, so they reuse
// both compiled shader libraries and only link. Pipelines own their shared libraries.
class StageLibraryCache {
public:
    using Owner = std::shared_ptr<vk::UniquePipeline>;
    static StageLibraryCache& Instance() { static StageLibraryCache cache; return cache; }
    Owner Find(vk::Device device, u64 key) { return libraries.Find({device, key}); }
    std::shared_future<Owner> Pending(vk::Device device, u64 key) {
        return libraries.Pending({device, key});
    }
    // Port of MR14 PERF-GT7-041, adapted to device-scoped owned libraries.
    // The shared creation cache also removes failed pending entries before retry.
    template<class Factory>
    std::pair<Owner, bool> GetOrCreate(vk::Device device, u64 key, bool share, Factory&& factory) {
        bool created = false;
        const auto create = [&] {
            created = true;
            return std::make_shared<vk::UniquePipeline>(factory(), device);
        };
        if (!share) return {create(), false};
        const bool joining = libraries.Pending({device, key}).valid();
        auto result = libraries.GetOrCreate({device, key}, create);
        if (joining && !created) shared_waits.fetch_add(1, std::memory_order_relaxed);
        if (!created) {
            const u64 avoided = duplicates_avoided.fetch_add(1, std::memory_order_relaxed) + 1;
            if (avoided <= 3 || avoided % 64 == 0)
                LOG_INFO(Render_Vulkan, "PERF-GT7-041: avoided {} duplicate stage-library compilations", avoided);
        }
        return {result, !created};
    }
    std::atomic<u64> shared_waits{};

private:
    SharedCreationCache<std::pair<vk::Device, u64>, vk::UniquePipeline> libraries;
    std::atomic<u64> duplicates_avoided{0};
};

class LibraryKey {
public:
    explicit LibraryKey(u64 kind) {
        Add(kind);
    }
    template <typename T>
    void Add(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* bytes = reinterpret_cast<const u8*>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(T));
    }
    u64 Hash() const {
        return XXH3_64bits(data.data(), data.size());
    }

private:
    std::vector<u8> data;
};

void GraphicsPipeline::SetStageInfos(std::span<const Shader::Info*, MaxShaderStages> infos) {
    std::ranges::copy(infos, stages.begin());
    std::ranges::copy(infos, canonical_stages.begin());
    // Installed before pipeline publication. Draw-stage copies change user data,
    // not this shader metadata. Unknown legacy metadata keeps the live scan.
    parallel_stage_summary = {};
    for (const auto* stage : GetStages()) if (stage) {
        parallel_stage_summary.known &= stage->attribute_flags_known && stage->resource_usage_known;
        parallel_stage_summary.safe &= stage->fmasks.empty() && !stage->translation_failed &&
            (stage->sw_stage == Shader::SwStage::Vertex || stage->sw_stage == Shader::SwStage::Fragment) &&
            (!stage->has_fetch_shader || stage->sw_stage == Shader::SwStage::Vertex) &&
            !stage->uses_patches && !stage->uses_dma && !stage->has_storage_images &&
            !stage->uses_shader_clock &&
            !std::ranges::any_of(stage->buffers, [](const auto& buffer) { return buffer.is_written; });
        parallel_stage_summary.vertex_fetch |= stage->has_fetch_shader;
        parallel_stage_summary.uses_direct |= stage->uses_direct;
    }
}

GraphicsPipeline::GraphicsPipeline(
    const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
    const Shader::Profile& profile, const GraphicsPipelineKey& key_,
    vk::PipelineCache pipeline_cache, std::span<const Shader::Info*, MaxShaderStages> infos,
    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    const Shader::Gcn::FetchShaderData* fetch_shader_, std::span<const vk::ShaderModule> modules,
    SerializationSupport& sdata, bool preloading)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache}, key{key_} {
    if (infos[u32(Shader::SwStage::TessellationControl)]) {
        const auto& tcs = runtime_infos[u32(Shader::SwStage::TessellationControl)].sw.tcs;
        lds_emulation = tcs.lds_emulation;
        lds_group_patches = tcs.num_patches;
        lds_vertex_bytes = ((tcs.ls_stride + 15) / 16) * 16;
    }
    if (fetch_shader_) {
        fetch_shader = *fetch_shader_;
    }

    const vk::Device device = instance.GetDevice();
    SetStageInfos(infos);
    // PERF-038: `preloading` builds a stored pipeline in the background. Its shader infos carry no
    // user data and there are no guest buffers to read: vertex inputs and the tessellation
    // helpers come from the stored serialization data; the runtime infos are the ones stored
    // with each stage's specialization, so the pipeline is the one a draw would build.
    BuildDescSetLayout(preloading);
    const auto debug_str = GetDebugString();
    BuildWatch watch{debug_str, preloading ? "stored pipeline build" : "pipeline build"};
    const bool tessellated = stages[u32(Shader::SwStage::TessellationControl)] != nullptr;

    const vk::PushConstantRange push_constants = {
        .stageFlags = AllGraphicsStageBits,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    // SHD-002: one set per stage; with libraries, sets are independent so each shader library
    // is made with its own stages' sets only.
    // SO-001: a pipeline drawn with shader objects gets a layout without independent sets: the
    // shaders are made with its set layouts and bound sets must match them exactly.
    // SO-003: tessellated ones too (also the rect/quad list emulation) where the device has
    // dynamic patch control points and domain origin.
    const bool so_tessellation = instance.IsShaderObjectTessellationEnabled();
    const bool so_candidate =
        ShaderObjectPolicyAllows(instance.IsShaderObjectEnabled(), lds_emulation, 0, 0) &&
        (so_tessellation || (!tessellated && key.prim_type != AmdGpu::PrimitiveType::RectList &&
                             key.prim_type != AmdGpu::PrimitiveType::QuadList)) &&
        !infos[u32(Shader::SwStage::Geometry)] &&
        // SO-004: GT7's pixel shaders nearly all ask for per-sample interpolation; with one
        // sample per pixel that changes nothing, so only multisampled ones stay pipelines.
        (key.num_samples <= 1 ||
         (!runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs.addr_flags.persp_sample_ena &&
          !runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs.addr_flags.linear_sample_ena)) &&
        key.depth_samples <= key.num_samples &&
        std::ranges::all_of(std::span{key.color_samples}.first(key.num_color_attachments),
                            [&](u8 samples) { return samples == 0 || samples == key.num_samples; });
    static const bool so_independent = [] {
        const char* env = std::getenv("RYGT_SO_INDEPENDENT");
        return !(env && env[0] == '0');
    }();
    const bool independent_sets =
        instance.IsGraphicsPipelineLibrarySupported() &&
        (!so_candidate || (so_independent && instance.IsShaderIndependentSetsSupported()));
    const vk::PipelineLayoutCreateInfo layout_info = {
        .flags = independent_sets ? vk::PipelineLayoutCreateFlagBits::eIndependentSetsEXT
                                  : vk::PipelineLayoutCreateFlags{},
        .setLayoutCount = num_sets,
        .pSetLayouts = set_layouts.data(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Graphics PipelineLayout {}", debug_str);

    VertexInputs<AmdGpu::Buffer> guest_buffers;
    if (!preloading && !instance.IsVertexInputDynamicState()) {
        const auto& vs_info = runtime_infos[u32(Shader::SwStage::Vertex)].sw.vs;
        GetVertexInputs(sdata.vertex_attributes, sdata.vertex_bindings, sdata.divisors,
                        guest_buffers, vs_info.step_rate_0, vs_info.step_rate_1);
    }

    const vk::PipelineVertexInputDivisorStateCreateInfo divisor_state = {
        .vertexBindingDivisorCount = static_cast<u32>(sdata.divisors.size()),
        .pVertexBindingDivisors = sdata.divisors.data(),
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .pNext = sdata.divisors.empty() ? nullptr : &divisor_state,
        .vertexBindingDescriptionCount = static_cast<u32>(sdata.vertex_bindings.size()),
        .pVertexBindingDescriptions = sdata.vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(sdata.vertex_attributes.size()),
        .pVertexAttributeDescriptions = sdata.vertex_attributes.data(),
    };

    const auto topology = LiverpoolToVK::PrimitiveType(key.prim_type);
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = topology,
    };

    const bool is_rect_list = key.prim_type == AmdGpu::PrimitiveType::RectList;
    const bool is_quad_list = key.prim_type == AmdGpu::PrimitiveType::QuadList;
    const vk::PipelineTessellationStateCreateInfo tessellation_state = {
        .patchControlPoints = is_rect_list ? 3U : (is_quad_list ? 4U : key.patch_control_points),
    };

    vk::StructureChain raster_chain = {
        vk::PipelineRasterizationStateCreateInfo{
            .depthClampEnable = key.depth_clamp_enable &&
                                (!key.depth_clip_enable || instance.IsDepthClipEnableSupported()),
            .rasterizerDiscardEnable = false,
            .polygonMode = LiverpoolToVK::PolygonMode(key.polygon_mode),
            .lineWidth = 1.0f,
        },
        vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT{
            .provokingVertexMode = key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First
                                       ? vk::ProvokingVertexModeEXT::eFirstVertex
                                       : vk::ProvokingVertexModeEXT::eLastVertex,
        },
        vk::PipelineRasterizationDepthClipStateCreateInfoEXT{
            .depthClipEnable = key.depth_clip_enable,
        },
    };

    if (!instance.IsProvokingVertexSupported()) {
        raster_chain.unlink<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>();
    }
    if (!instance.IsDepthClipEnableSupported()) {
        raster_chain.unlink<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>();
    }

    const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
    // FIX-220: alpha to coverage needs an alpha at location 0; a pixel shader that writes none
    // draws as without it.
    const Shader::Info* const fragment_stage = stages[u32(Shader::SwStage::Fragment)];
    const bool writes_coverage_alpha =
        fragment_stage && fragment_stage->stores.Get(Shader::IR::Attribute::RenderTarget0, 3);
    sdata.multisampling = {
        .rasterizationSamples = LiverpoolToVK::NumSamples(
            key.num_samples, instance.GetColorSampleCounts() & instance.GetDepthSampleCounts()),
        .sampleShadingEnable =
            fs_info.addr_flags.persp_sample_ena || fs_info.addr_flags.linear_sample_ena,
        .alphaToCoverageEnable = key.alpha_to_coverage && writes_coverage_alpha,
    };

    const vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control = {
        // FIX-GTS-443: 0..1 when the vertex shader converts the depth. A geometry shader's
        // pipeline keeps -1..1: only the hardware vertex stage converts, and with a geometry
        // shader the vertex shader runs as the export stage. (Shader objects never have one.)
        .negativeOneToOne = key.clip_space == AmdGpu::ClipSpace::MinusWToW &&
                            (instance.UsesDepthClipControl() ||
                             infos[u32(Shader::SwStage::Geometry)] != nullptr),
    };

    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .pNext = instance.IsDepthClipControlSupported() ? &clip_control : nullptr,
    };

    boost::container::static_vector<vk::DynamicState, 32> dynamic_states = {
        vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
        vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
        vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,          vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::eLineWidth,          vk::DynamicState::ePrimitiveRestartEnable,
    };

    if (instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dynamic_states.push_back(vk::DynamicState::eAttachmentFeedbackLoopEnableEXT);
    }
    if (instance.IsDepthBoundsSupported()) {
        dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
        dynamic_states.push_back(vk::DynamicState::eDepthBounds);
    }
    if (instance.IsDynamicColorWriteMaskSupported()) {
        dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (instance.IsVertexInputDynamicState()) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);
    } else if (!sdata.vertex_bindings.empty()) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputBindingStride);
    }

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages>
        shader_stages;
    auto stage = u32(Shader::SwStage::Vertex);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::Geometry);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eGeometry,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::TessellationControl);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        const auto type = is_quad_list ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS;
        const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
        sdata.tcs = Shader::Backend::SPIRV::EmitAuxilaryTessShader(
            type, fs_info, *infos[u32(Shader::SwStage::Vertex)],
            infos[u32(Shader::SwStage::Fragment)]);
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = CompileSPV(sdata.tcs, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::TessellationEval);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
        sdata.tes = Shader::Backend::SPIRV::EmitAuxilaryTessShader(
            AuxShaderType::PassthroughTES, fs_info, *infos[u32(Shader::SwStage::Vertex)],
            infos[u32(Shader::SwStage::Fragment)]);
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = CompileSPV(sdata.tes, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::Fragment);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs.clip_distance_emulation) {
        const auto& vs = runtime_infos[static_cast<u32>(Shader::SwStage::Vertex)].hw.vs;

        sdata.fragment = Shader::Backend::SPIRV::EmitDiscardFragmentShader(vs.outputs);
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = CompileSPV(sdata.fragment, instance.GetDevice()),
            .pName = "main",
        });
    }

    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = 64,
    };
    // AMD-301: only stages the driver lists in requiredSubgroupSizeStages take the requirement
    // (fragment from RDNA 3 on; compute only on GCN, RDNA 1 and 2, where vertex never did).
    const vk::ShaderStageFlags sized_stages = instance.RequiredSubgroupSizeStages();
    for (auto& stage : shader_stages) {
        stage.pNext = instance.IsSubgroupSize64Supported() && (sized_stages & stage.stage)
                          ? &subgroup_size_ci
                          : nullptr;
    }

    const auto depth_format =
        instance.GetSupportedFormat(LiverpoolToVK::DepthFormat(key.z_format, key.stencil_format),
                                    vk::FormatFeatureFlagBits2::eDepthStencilAttachment);
    std::array<vk::Format, Shader::IR::NumRenderTargets> color_formats;
    for (s32 i = 0; i < key.num_color_attachments; ++i) {
        const auto& col_buf = key.color_buffers[i];
        const auto format = LiverpoolToVK::SurfaceFormat(col_buf.data_format, col_buf.num_format);
        auto color_format =
            instance.GetSupportedFormat(format, vk::FormatFeatureFlagBits2::eColorAttachment);
        if (col_buf.hdr_ui_layer) {
            color_format = vk::Format::eR16G16B16A16Sfloat; // ryGT HDR-UI: the UI layer
        }
        if (!instance.IsFormatSupported(color_format,
                                        vk::FormatFeatureFlagBits2::eColorAttachment)) {
            LOG_WARNING(Render_Vulkan,
                        "color buffer format {} does not support COLOR_ATTACHMENT_BIT",
                        vk::to_string(color_format));
        }
        color_formats[i] = color_format;
    }

    std::array<vk::SampleCountFlagBits, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    std::ranges::transform(key.color_samples, color_samples.begin(), [&instance](u8 num_samples) {
        return num_samples ? LiverpoolToVK::NumSamples(num_samples, instance.GetColorSampleCounts())
                           : vk::SampleCountFlagBits::e1;
    });
    const vk::AttachmentSampleCountInfoAMD mixed_samples = {
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentSamples = color_samples.data(),
        .depthStencilAttachmentSamples =
            LiverpoolToVK::NumSamples(key.depth_samples, instance.GetDepthSampleCounts()),
    };

    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci = {
        .pNext = instance.IsMixedDepthSamplesSupported() ? &mixed_samples : nullptr,
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentFormats = color_formats.data(),
        .depthAttachmentFormat = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid
                                     ? depth_format
                                     : vk::Format::eUndefined,
        .stencilAttachmentFormat = key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid
                                       ? depth_format
                                       : vk::Format::eUndefined,
    };

    std::array<vk::PipelineColorBlendAttachmentState, AmdGpu::NUM_COLOR_BUFFERS> attachments;
    for (u32 i = 0; i < key.num_color_attachments; i++) {
        const auto& control = key.blend_controls[i];

        const auto src_color = LiverpoolToVK::BlendFactor(control.color_src_factor);
        const auto dst_color = LiverpoolToVK::BlendFactor(control.color_dst_factor);
        const auto color_blend = LiverpoolToVK::BlendOp(control.color_func);

        const auto src_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_src_factor)
                                   : src_color;
        const auto dst_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_dst_factor)
                                   : dst_color;
        const auto alpha_blend =
            control.separate_alpha_blend ? LiverpoolToVK::BlendOp(control.alpha_func) : color_blend;

        // Vulkan ignores blend factors for min/max, but a factor that zeroes one operand
        // makes the operation collapse to a plain selection: min(s, 0) is 0 and max(s, 0)
        // is s for normalized alpha. Rewrite those to the equivalent add so the result is
        // exact instead of leaving the other operand to survive.
        auto eff_src_alpha = src_alpha;
        auto eff_dst_alpha = dst_alpha;
        auto eff_alpha_blend = alpha_blend;
        if (alpha_blend == vk::BlendOp::eMin || alpha_blend == vk::BlendOp::eMax) {
            const bool takes_max = alpha_blend == vk::BlendOp::eMax;
            if (src_alpha == vk::BlendFactor::eOne && dst_alpha == vk::BlendFactor::eZero) {
                eff_alpha_blend = vk::BlendOp::eAdd;
                eff_src_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
                eff_dst_alpha = vk::BlendFactor::eZero;
            } else if (src_alpha == vk::BlendFactor::eZero && dst_alpha == vk::BlendFactor::eOne) {
                eff_alpha_blend = vk::BlendOp::eAdd;
                eff_src_alpha = vk::BlendFactor::eZero;
                eff_dst_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
            }
        }

        const auto color_scaled_min_max =
            (color_blend == vk::BlendOp::eMin || color_blend == vk::BlendOp::eMax) &&
            (src_color != vk::BlendFactor::eOne || dst_color != vk::BlendFactor::eOne) &&
            !key.color_buffers[i].blend_self_scale;
        const auto alpha_scaled_min_max =
            (eff_alpha_blend == vk::BlendOp::eMin || eff_alpha_blend == vk::BlendOp::eMax) &&
            (eff_src_alpha != vk::BlendFactor::eOne || eff_dst_alpha != vk::BlendFactor::eOne);
        if (color_scaled_min_max || alpha_scaled_min_max) {
            LOG_WARNING(
                Render_Vulkan,
                "Unimplemented use of min/max blend op with blend factor not equal to one.");
        }

        attachments[i] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = control.enable,
            .srcColorBlendFactor = src_color,
            .dstColorBlendFactor = dst_color,
            .colorBlendOp = color_blend,
            .srcAlphaBlendFactor = eff_src_alpha,
            .dstAlphaBlendFactor = eff_dst_alpha,
            .alphaBlendOp = eff_alpha_blend,
            .colorWriteMask =
                instance.IsDynamicColorWriteMaskSupported()
                    ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
                    : key.write_masks[i],
        };

        // ryGT HDR-UI: the UI layer's alpha is its coverage over the scene ("over").
        if (key.color_buffers[i].hdr_ui_layer) {
            attachments[i].srcAlphaBlendFactor = vk::BlendFactor::eOne;
            attachments[i].dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
            attachments[i].alphaBlendOp = vk::BlendOp::eAdd;
            attachments[i].colorWriteMask |= vk::ColorComponentFlagBits::eA;
        }

        // The shader squares its color output for this attachment (see PsColorBuffer), so the
        // factors must not scale the operands again.
        if (key.color_buffers[i].blend_self_scale) {
            LOG_WARNING(
                Render_Vulkan,
                "Emulating scaled min/max blend with squared shader output on attachment {}", i);
            attachments[i].srcColorBlendFactor = vk::BlendFactor::eOne;
            attachments[i].dstColorBlendFactor = vk::BlendFactor::eOne;
        }

        // On GCN GPU there is an additional mask which allows to control color components exported
        // from a pixel shader. A situation possible, when the game may mask out the alpha channel,
        // while it is still need to be used in blending ops. For such cases, HW will default alpha
        // to 1 and perform the blending, while shader normally outputs 0 in the last component.
        // Unfortunatelly, Vulkan doesn't provide any control on blend inputs, so below we detecting
        // such cases and override alpha value in order to emulate HW behaviour.
        const auto has_alpha_masked_out =
            (key.cb_shader_mask.GetMask(i) & AmdGpu::ColorBufferMask::ComponentA) == 0;
        const auto has_src_alpha_in_src_blend = src_color == vk::BlendFactor::eSrcAlpha ||
                                                src_color == vk::BlendFactor::eOneMinusSrcAlpha;
        const auto has_src_alpha_in_dst_blend = dst_color == vk::BlendFactor::eSrcAlpha ||
                                                dst_color == vk::BlendFactor::eOneMinusSrcAlpha;
        if (has_alpha_masked_out && has_src_alpha_in_src_blend) {
            attachments[i].srcColorBlendFactor = src_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
        if (has_alpha_masked_out && has_src_alpha_in_dst_blend) {
            attachments[i].dstColorBlendFactor = dst_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }

        if (key.color_buffers[i].blend_swizzled_alpha) {
            LiverpoolToVK::SetSwizzledAlphaBlend(attachments[i]);
            static const bool logged = [] {
                LOG_INFO(Render_Vulkan, "Using dual-source swizzled source-alpha blend emulation");
                return true;
            }();
        } else if (key.color_buffers[i].blend_swizzled_factors) {
            // Both functions match for this emulation.
            LiverpoolToVK::SetSwizzledFactorBlend(attachments[i], control.color_func);
            static const bool logged = [] {
                LOG_INFO(Render_Vulkan, "Using dual-source swizzled factor blend emulation");
                return true;
            }();
        }
    }

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable =
            instance.IsLogicOpSupported() && key.logic_op != AmdGpu::ColorControl::LogicOp::Copy,
        .logicOp = LiverpoolToVK::LogicOp(key.logic_op),
        .attachmentCount = key.num_color_attachments,
        .pAttachments = attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    // Required by spec unless VK_EXT_extended_dynamic_state3 is supported.
    // In practice, we use dynamic state for all of it.
    constexpr vk::PipelineDepthStencilStateCreateInfo depth_stencil_info = {};

    // SO-017: GT7 HUD and menu blending use conventional pipelines by default.
    // FIX-GTS-449: not on NVIDIA. There a shader-object draw that follows pipeline draws can lose
    // its colour output while its depth is written (GT Sport's garage floor drew black, and grass
    // tufts showed through the road in races): state valid by the spec, validation clean, AMD
    // correct. With blended draws on shader objects too, nearly no draw follows a pipeline, and
    // both were gone (caches off, Nürburgring start). SO-015 had already fixed the HUD sprites
    // SO-017 was for. RYGT_SO_SKIP still overrides.
    static const u32 so_skip = [vendor = instance.GetVendorID()] {
        const char* env = std::getenv("RYGT_SO_SKIP");
        u32 bits = env ? u32(std::strtoul(env, nullptr, 0)) : (vendor == 0x10de ? 0u : 1u);
        if (std::FILE* file = std::fopen("so_skip.txt", "r")) {
            char text[32]{};
            if (std::fgets(text, sizeof(text), file)) bits = u32(std::strtoul(text, nullptr, 0));
            std::fclose(file);
        }
        return bits;
    }();
    u32 kinds = 0;
    for (u32 i = 0; i < key.num_color_attachments; ++i) {
        const auto& a = attachments[i];
        if (a.blendEnable) kinds |= 1;
        const auto src1 = [](vk::BlendFactor f) {
            return f == vk::BlendFactor::eSrc1Color || f == vk::BlendFactor::eSrc1Alpha ||
                   f == vk::BlendFactor::eOneMinusSrc1Color || f == vk::BlendFactor::eOneMinusSrc1Alpha;
        };
        if (src1(a.srcColorBlendFactor) || src1(a.dstColorBlendFactor) ||
            src1(a.srcAlphaBlendFactor) || src1(a.dstAlphaBlendFactor)) kinds |= 8;
        if (key.write_masks[i] != (vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA)) kinds |= 32;
    }
    if (sdata.multisampling.alphaToCoverageEnable) kinds |= 2;
    if (color_blending.logicOpEnable) kinds |= 4;
    if (!infos[u32(Shader::SwStage::Fragment)]) kinds |= 16;
    // DIAG-GTS-431: RYGT_SO_SKIP_HASHES=<hex,hex,...> keeps pipelines containing any of these
    // shader programs (any stage) off shader objects, to test one draw's shaders on their own.
    static const std::vector<u64> so_skip_hashes = [] {
        std::vector<u64> hashes;
        if (const char* env = std::getenv("RYGT_SO_SKIP_HASHES"); env && *env) {
            std::string list{env};
            for (size_t pos = 0; pos < list.size();) {
                const size_t end = std::min(list.find(',', pos), list.size());
                hashes.push_back(std::strtoull(list.substr(pos, end - pos).c_str(), nullptr, 16));
                pos = end + 1;
            }
            LOG_WARNING(Render_Vulkan, "DIAG-GTS-431: {} shader programs kept off shader objects",
                        hashes.size());
        }
        return hashes;
    }();
    const bool hash_skipped = !so_skip_hashes.empty() && std::ranges::any_of(infos, [](const auto* info) {
        return info && std::ranges::find(so_skip_hashes, info->pgm_hash) != so_skip_hashes.end();
    });
    // DIAG-GTS-432: RYGT_SO_HASH_MASK / RYGT_SO_HASH_VALUE (hex) let only pipelines whose mixed
    // shader hash h has (h & mask) == value use shader objects, to bisect which pipeline's
    // shader objects break something. The mixed hash and the decision are logged per pipeline.
    // DIAG-GTS-440: RYGT_SO_HASH_TERMS="mask:value,mask:value,..." (hex) is the same filter with
    // several terms; a pipeline uses shader objects when any term matches. It replaces the
    // MASK/VALUE pair when set, so two groups of pipelines can be bisected at once.
    static const std::vector<std::pair<u64, u64>> so_hash_filter = [] {
        std::vector<std::pair<u64, u64>> terms;
        if (const char* list = std::getenv("RYGT_SO_HASH_TERMS"); list && *list) {
            const char* p = list;
            while (*p) {
                char* end = nullptr;
                const u64 m = std::strtoull(p, &end, 16);
                if (end == p) break;
                p = end;
                u64 v = 0;
                if (*p == ':') {
                    v = std::strtoull(p + 1, &end, 16);
                    p = end;
                }
                if (m) terms.emplace_back(m, v & m);
                while (*p == ',' || *p == ' ' || *p == ';') ++p;
            }
        } else {
            const char* mask = std::getenv("RYGT_SO_HASH_MASK");
            const char* value = std::getenv("RYGT_SO_HASH_VALUE");
            const u64 m = mask && *mask ? std::strtoull(mask, nullptr, 16) : 0;
            const u64 v = value && *value ? std::strtoull(value, nullptr, 16) : 0;
            if (m) terms.emplace_back(m, v);
        }
        for (const auto& [m, v] : terms) {
            LOG_WARNING(Render_Vulkan, "DIAG-GTS-432: shader objects where (h & {:#x}) == {:#x}", m, v);
        }
        return terms;
    }();
    bool hash_filtered = false;
    if (!so_hash_filter.empty()) {
        u64 h = 0x9E3779B97F4A7C15ULL;
        for (const auto* info : infos) {
            h = (h ^ (info ? info->pgm_hash : 0)) * 0x100000001B3ULL;
            h ^= h >> 29;
        }
        hash_filtered = std::ranges::none_of(
            so_hash_filter, [h](const auto& t) { return (h & t.first) == t.second; });
        if (so_candidate) {
            LOG_INFO(Render_Vulkan, "DIAG-GTS-432: {} h={:#018x} {}", GetDebugString(), h,
                     hash_filtered ? "pipeline" : "shader objects");
        }
    }
    if (!hash_skipped && !hash_filtered &&
        ShaderObjectPolicyAllows(so_candidate, lds_emulation, kinds, so_skip) &&
        (key.num_samples <= 1 || !sdata.multisampling.sampleShadingEnable)) {
        const auto& raster = raster_chain.get<vk::PipelineRasterizationStateCreateInfo>();
        so_state.topology = topology;
        so_state.polygon_mode = raster.polygonMode;
        so_state.samples = sdata.multisampling.rasterizationSamples;
        so_state.provoking = key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First
                                 ? vk::ProvokingVertexModeEXT::eFirstVertex
                                 : vk::ProvokingVertexModeEXT::eLastVertex;
        so_state.depth_clamp = raster.depthClampEnable != VK_FALSE;
        so_state.depth_clip = key.depth_clip_enable != 0;
        so_state.negative_one_to_one = key.clip_space == AmdGpu::ClipSpace::MinusWToW &&
                                       instance.UsesDepthClipControl(); // FIX-GTS-443
        // SO-015: as the pipeline's: only when the pixel shader writes alpha to target 0 (else
        // the coverage came from an undefined alpha: HUD sprites and the menu cursor vanished).
        so_state.alpha_to_coverage = sdata.multisampling.alphaToCoverageEnable != VK_FALSE;
        so_state.logic_op_enable = color_blending.logicOpEnable != VK_FALSE;
        so_state.logic_op = color_blending.logicOp;
        so_state.attachments = key.num_color_attachments;
        so_state.patch_control_points = tessellated || is_rect_list || is_quad_list
                                            ? tessellation_state.patchControlPoints : 0;
        // DIAG-GTS-442: RYGT_SO_CLIP_SPACE=0 forces shader-object draws to 0..1 depth, 1 to
        // -1..1 (default: the guest's clip space, as the pipeline). With the hash filter on, the
        // chosen pipelines' depth state is logged: a shader-object draw wrote depth slightly
        // nearer than the same surface drawn by a pipeline (garage floor, grass on the road).
        static const int so_clip_space = [] {
            const char* env = std::getenv("RYGT_SO_CLIP_SPACE");
            return env && (env[0] == '0' || env[0] == '1') ? env[0] - '0' : -1;
        }();
        if (so_clip_space >= 0) {
            so_state.negative_one_to_one = so_clip_space == 1;
        }
        if (!so_hash_filter.empty()) {
            LOG_WARNING(Render_Vulkan,
                        "DIAG-GTS-442: {} clip_space={} neg1to1={} clamp={} (key {}) clip={} "
                        "clip_control={} clip_enable_ext={} samples={} polygon={} provoking={} "
                        "topology={}",
                        GetDebugString(), u32(key.clip_space), so_state.negative_one_to_one,
                        so_state.depth_clamp, u32(key.depth_clamp_enable), so_state.depth_clip,
                        instance.IsDepthClipControlSupported(),
                        instance.IsDepthClipEnableSupported(), u32(so_state.samples),
                        u32(so_state.polygon_mode), u32(so_state.provoking),
                        u32(so_state.topology));
        }
        if (CreateShaderObjects(modules, sdata, attachments,
                                infos[u32(Shader::SwStage::Fragment)] != nullptr,
                                independent_sets)) return;
    }

#ifndef NDEBUG
    if (instance.IsShaderObjectEnabled()) {
        static std::atomic<u32> rejected{};
        if (const u32 n = ++rejected; n <= 30 || n % 200 == 0)
            LOG_WARNING(Render_Vulkan, "SO-004: {} used a graphics pipeline ({} rejections)", GetDebugString(), n);
    }
#endif
    if (so_candidate && instance.IsGraphicsPipelineLibrarySupported()) {
        auto layout_ci = layout_info;
        layout_ci.flags = vk::PipelineLayoutCreateFlagBits::eIndependentSetsEXT;
        auto [result, independent] = instance.GetDevice().createPipelineLayoutUnique(layout_ci);
        ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create graphics pipeline layout: {}",
                   vk::to_string(result));
        pipeline_layout = std::move(independent);
    }

    // Protect both library and whole-pipeline tessellation builds from the optimizer.
    const bool amd_policy = AmdPipelinePolicyEnabled(instance.GetVendorID()); // AMD-320
    const bool plain_tessellated = tessellated && !amd_policy;
    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = &pipeline_rendering_ci,
        .flags = tessellated && plain_tessellated ? vk::PipelineCreateFlagBits::eDisableOptimization
                                                  : vk::PipelineCreateFlags{},
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = !instance.IsVertexInputDynamicState() ? &vertex_input_info : nullptr,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = &tessellation_state,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_chain.get(),
        .pMultisampleState = &sdata.multisampling,
        .pDepthStencilState =
            !instance.IsExtendedDynamicState3Supported() ? &depth_stencil_info : nullptr,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
    };

    const auto create_start = std::chrono::steady_clock::now();
    // Stored ordinary pipelines use the same shared stage libraries and fast link as draws.
    // Whole-pipeline prewarm recompiles both stages for every target/blend permutation and
    // does not populate the stage-library cache used when new runtime permutations appear.
    const auto compile_policy = amd_policy
        ? SharedPipelinePolicy::SelectAmd(
              SharedPipelineLibrariesEnabled(instance.IsGraphicsPipelineLibrarySupported(),preloading),
              tessellated && (AmdWholeTessellation() || !Mr14SharedTessellationEnabled(false)))
        : SharedPipelinePolicy::Select(
              SharedPipelineLibrariesEnabled(instance.IsGraphicsPipelineLibrarySupported(),preloading),
              tessellated && (Common::PerfFeatureEnabled(80) || !Mr14SharedTessellationEnabled(LinkTimeOptimization())),
              tessellated, LinkTimeOptimization(), is_rect_list || is_quad_list);
    // AMD-322 / SHD-001: whether libraries keep link-time data for a second, optimized link.
    const bool lto_relink = compile_policy.background_optimization;
    if (compile_policy.libraries) {
        // PERF-017: a pipeline first needed during play is built from four separately compiled
        // libraries and linked without cross-stage optimization, which the driver does in a
        // fraction of a full compile. A worker then links the same libraries with link-time
        // optimization, and draws switch to that pipeline once it exists. Both pipelines come
        // from the same create info, so they render the same.
        if (tessellated) {
            static std::atomic<u32> tess_library_builds{};
            const auto count = ++tess_library_builds;
            if (count <= 8 || count % 128 == 0)
                LOG_INFO(Render_Vulkan, "PERF-GT7-040: {} uses shared tessellation libraries, optimizer disabled, startup={} ({} builds)", debug_str, preloading, count);
        }
        library_link = std::make_unique<LibraryLink>();
        boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages>
            pre_raster_stages;
        boost::container::static_vector<vk::PipelineShaderStageCreateInfo, 1> fragment_stages;
        for (const auto& stage_info : shader_stages) {
            if (stage_info.stage == vk::ShaderStageFlagBits::eFragment) {
                fragment_stages.push_back(stage_info);
            } else {
                pre_raster_stages.push_back(stage_info);
            }
        }
        // SHD-002: the shader libraries' layouts hold their own stages' sets only (the others
        // are null), so a library doesn't depend on the other stages' resources.
        const auto library_layout = [&](bool fragment) {
            std::array<vk::DescriptorSetLayout, Shader::Backend::MaxDescriptorSets> sets{};
            u32 count = 0;
            for (u32 set = 0; set < num_sets; ++set) {
                const bool fragment_set = set == Shader::Backend::StageSet(
                                                     u32(Shader::SwStage::Fragment));
                if (fragment_set == fragment && set_layout_hashes[set] != 0) {
                    sets[set] = set_layouts[set];
                    count = set + 1;
                }
            }
            auto [result, created] = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{
                .flags = vk::PipelineLayoutCreateFlagBits::eIndependentSetsEXT,
                .setLayoutCount = count,
                .pSetLayouts = sets.data(),
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &push_constants,
            });
            ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create a library layout: {}",
                       vk::to_string(result));
            return std::move(created);
        };
        const vk::UniquePipelineLayout pre_raster_layout = library_layout(false);
        const vk::UniquePipelineLayout fragment_layout = library_layout(true);
        const std::array<vk::GraphicsPipelineLibraryFlagsEXT, 4> parts = {
            vk::GraphicsPipelineLibraryFlagBitsEXT::eVertexInputInterface,
            vk::GraphicsPipelineLibraryFlagBitsEXT::ePreRasterizationShaders,
            vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentShader,
            vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentOutputInterface,
        };
        std::array<vk::GraphicsPipelineLibraryCreateInfoEXT, 4> library_infos{};
        std::array<vk::GraphicsPipelineCreateInfo, 4> part_infos{};
        for (size_t i = 0; i < parts.size(); ++i) {
            library_infos[i] = vk::GraphicsPipelineLibraryCreateInfoEXT{
                .pNext = &pipeline_rendering_ci,
                .flags = parts[i],
            };
            auto& part_info = part_infos[i];
            part_info = pipeline_info;
            part_info.pNext = &library_infos[i];
            part_info.flags = pipeline_info.flags | vk::PipelineCreateFlagBits::eLibraryKHR;
            if (compile_policy.background_optimization) {
                part_info.flags |= vk::PipelineCreateFlagBits::eRetainLinkTimeOptimizationInfoEXT;
            }
            part_info.stageCount = 0;
            part_info.pStages = nullptr;
            if (i == 1) {
                part_info.stageCount = static_cast<u32>(pre_raster_stages.size());
                part_info.pStages = pre_raster_stages.data();
                part_info.layout = *pre_raster_layout; // SHD-002
            } else if (i == 2) {
                part_info.stageCount = static_cast<u32>(fragment_stages.size());
                part_info.pStages = fragment_stages.data();
                part_info.layout = *fragment_layout; // SHD-002
            }
        }
        // PERF-GT7-108: a shader library the driver compiled, kept as a compile pack recipe.
        const auto record_library = [&](size_t i) {
            CompilePack::LayoutDesc layout{
                .flags = static_cast<u32>(VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT),
                .push_stages =
                    static_cast<u32>(static_cast<VkShaderStageFlags>(push_constants.stageFlags)),
                .push_size = push_constants.size,
            };
            const bool fragment = i == 2;
            for (u32 set = 0; set < num_sets; ++set) { // as library_layout (SHD-002)
                const bool fragment_set =
                    set == Shader::Backend::StageSet(u32(Shader::SwStage::Fragment));
                if (fragment_set != fragment || set_layout_hashes[set] == 0) {
                    continue;
                }
                layout.sets.resize(set + 1);
                CompilePack::SetLayout desc;
                desc.flags = set == push_set
                                 ? static_cast<u32>(VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR)
                                 : 0u;
                for (const auto& binding : pack_bindings[set]) {
                    if (binding.pImmutableSamplers) {
                        return;
                    }
                    desc.bindings.push_back(
                        {binding.binding, static_cast<u32>(binding.descriptorType),
                         binding.descriptorCount,
                         static_cast<u32>(static_cast<VkShaderStageFlags>(binding.stageFlags))});
                }
                layout.sets[set] = std::move(desc);
            }
            std::vector<u8> raw;
            if (!CompilePack::SerializeLibrary(
                    part_infos[i], layout, [&](vk::ShaderModule m) { return ModuleCode(device, m); }, raw,
                    false)) {
                static std::atomic<u32> refused{};
                if (++refused <= 5) {
                    LOG_INFO(Render_Vulkan, "PERF-GT7-108: {} library of {} not kept (state not known)",
                             fragment ? "fragment" : "pre-raster", debug_str);
                }
                return;
            }
            static const auto pack_dir = CompilePack::Dir(
                Common::FS::GetUserPath(Common::FS::PathType::CacheDir),
                Common::Singleton<Common::ElfInfo>::Instance()->GameSerial(), instance.GetVendorID(),
                instance.GetDeviceID());
            const auto file =
                pack_dir / fmt::format("lib-{:016x}.pack", XXH3_64bits(raw.data(), raw.size()));
            std::error_code ec;
            static std::atomic<u32> kept{};
            if (!std::filesystem::exists(file, ec) && CompilePack::WriteLibraryFile(file, raw)) {
                if (const u32 n = ++kept; n == 1 || n % 200 == 0) {
                    LOG_INFO(Render_Vulkan, "PERF-GT7-108: {} library recipes kept", n);
                }
            }
        };
        const auto create_library = [&](size_t i) {
            // Only this subset contains the tessellation shaders. Interface/fragment
            // compilation does not need to monopolize the heavy admission token.
            HeavyBuildGate gate{tessellated && i == 1};
            BuildWatch part_watch{debug_str, i == 1 ? "pre-raster library"
                                             : i == 2 ? "fragment library"
                                                      : "interface library"};
            auto [library_result, library] = PipelineBinaryStore::CreateGraphics(
                instance, device, pipeline_cache, part_infos[i]); // BIN-001
            // AMD-OOM-001: AMD's compiler ran out of system memory (ErrorOutOfHostMemory,
            // "LLVM ERROR: out of memory") with several builds at once, and the assertion below
            // ended GT Sport. Wait, then retry alone (the heavy-build slot), up to six times.
            for (int attempt = 1;
                 attempt <= 6 && (library_result == vk::Result::eErrorOutOfHostMemory ||
                                  library_result == vk::Result::eErrorOutOfDeviceMemory);
                 ++attempt) {
                LOG_WARNING(Render_Vulkan,
                            "AMD-OOM-001: {} library of {} out of memory ({}); retry {} alone",
                            i == 1 ? "pre-raster" : i == 2 ? "fragment" : "interface", debug_str,
                            vk::to_string(library_result), attempt);
                std::this_thread::sleep_for(std::chrono::milliseconds{250 * attempt});
                HeavyBuildGate alone{!(tessellated && i == 1)};
                auto retried = PipelineBinaryStore::CreateGraphics(instance, device,
                                                                   pipeline_cache, part_infos[i]);
                library_result = retried.result;
                library = retried.value;
            }
            if (library_result != vk::Result::eSuccess) {
                auto retried = CreateWithRetries("library of", debug_str, [&] {
                    return PipelineBinaryStore::CreateGraphics(instance, device, pipeline_cache,
                                                               part_infos[i]);
                }); // FIX-GT7-101
                library_result = retried.result;
                library = retried.value;
            }
            ASSERT_MSG(library_result == vk::Result::eSuccess,
                       "Failed to create graphics pipeline library: {}",
                       vk::to_string(library_result));
            // FIX-GT7-136: a library recipe holds its stages' SPIR-V, kept only with shader
            // objects on (without them every recipe was refused as "state not known").
            if ((i == 1 || i == 2) && CompilePack::RecordingOn() &&
                instance.IsShaderObjectEnabled()) {
                record_library(i);
            }
            return library;
        };

        // PERF-018: the shader libraries are keyed by everything their create info reads: the
        // layout's bindings, the stage modules, and the state of their pipeline subset.
        LibraryKey pre_raster_key{1};
        pre_raster_key.Add(part_infos[1].flags);
        pre_raster_key.Add(pipeline_rendering_ci.viewMask);
        // SHD-002: the sets of the pre-rasterization stages only.
        for (u32 set = 0; set < num_sets; ++set) {
            if (set != Shader::Backend::StageSet(u32(Shader::SwStage::Fragment))) {
                pre_raster_key.Add(set_layout_hashes[set]);
            }
        }
        for (const auto& stage_info : pre_raster_stages) {
            pre_raster_key.Add(stage_info.stage);
            pre_raster_key.Add(static_cast<VkShaderModule>(stage_info.module));
        }
        pre_raster_key.Add(tessellation_state.patchControlPoints);
        pre_raster_key.Add(
            raster_chain.get<vk::PipelineRasterizationStateCreateInfo>().depthClampEnable);
        pre_raster_key.Add(key.polygon_mode);
        pre_raster_key.Add(key.provoking_vtx_last);
        pre_raster_key.Add(key.depth_clip_enable);
        pre_raster_key.Add(key.clip_space);
        for (const auto state : dynamic_states) {
            pre_raster_key.Add(state);
        }

        // AMD-320: libraries with and without link-time data are different libraries.
        pre_raster_key.Add(lto_relink);
        LibraryKey fragment_key{2};
        fragment_key.Add(part_infos[2].flags);
        // SHD-002: the fragment stage's set only.
        fragment_key.Add(set_layout_hashes[Shader::Backend::StageSet(u32(Shader::SwStage::Fragment))]);
        for (const auto& stage_info : fragment_stages) {
            fragment_key.Add(static_cast<VkShaderModule>(stage_info.module));
        }
        // Keep FS/fragment-output multisampling identical even without sample
        // shading. The old key could link a cached 1x FS library to 4x output.
        AddFragmentMultisampleKey(fragment_key, sdata.multisampling);
        fragment_key.Add(pipeline_rendering_ci.viewMask);
        // FIX-GT7-030: tessellated pipelines' libraries are not optimized (FIX-057): the others must
        // not draw with them.
        fragment_key.Add(lto_relink);
        fragment_key.Add(static_cast<bool>(pipeline_info.flags &
                                           vk::PipelineCreateFlagBits::eDisableOptimization));
        for (const auto state : dynamic_states) {
            fragment_key.Add(state);
        }

        auto& library_cache = StageLibraryCache::Instance();
        const u64 pre_raster_hash = pre_raster_key.Hash();
        const u64 fragment_hash = fragment_key.Hash();
        // -DisablePerf 18 compiles every library for every pipeline, one after another.
        const bool share_libraries = Common::PerfFeatureEnabled(18);
        auto pre_raster_owner = share_libraries ? library_cache.Find(device, pre_raster_hash) : nullptr;
        auto fragment_owner = share_libraries ? library_cache.Find(device, fragment_hash) : nullptr;
        vk::Pipeline pre_raster_library = pre_raster_owner ? **pre_raster_owner : vk::Pipeline{};
        vk::Pipeline fragment_library = fragment_owner ? **fragment_owner : vk::Pipeline{};
        bool pre_raster_reused = bool(pre_raster_library);
        bool fragment_reused = bool(fragment_library);
        // Pipeline workers already provide parallelism across pipelines. A nested
        // std::async per fragment bypasses that budget and starts default-priority
        // compilers alongside the low-priority workers. Build both stages on this
        // worker; Acquire still shares in-flight and completed libraries.
        std::array<vk::Pipeline, 4> library_handles{};
        library_handles[0] = create_library(0);
        library_handles[3] = create_library(3);
        // FIX-419-GTS (follows FIX-GT7-030's may_wait): a build a draw waits for
        // (g_pipeline_build_urgent) makes its own copy of a library another build is making.
        const auto may_share = [&](u64 hash) {
            return share_libraries &&
                   !(g_pipeline_build_urgent && library_cache.Pending(device, hash).valid());
        };
        // Fill independent fragments on the existing worker pool before waiting for
        // the single tessellation compiler. No nested threads, no extra heavy jobs.
        if (!fragment_library) {
            auto [owner, reused] = library_cache.GetOrCreate(device, fragment_hash, may_share(fragment_hash),
                [&] { return create_library(2); });
            fragment_owner = std::move(owner);
            fragment_reused = reused;
            fragment_library = **fragment_owner;
        }
        if (!pre_raster_library) {
            auto [owner, reused] = library_cache.GetOrCreate(device, pre_raster_hash, may_share(pre_raster_hash),
                [&] { return create_library(1); });
            pre_raster_owner = std::move(owner);
            pre_raster_reused = reused;
            pre_raster_library = **pre_raster_owner;
        }
        library_link->shared_libraries = {pre_raster_owner, fragment_owner};
        library_handles[1] = pre_raster_library;
        library_handles[2] = fragment_library;
        library_link->owned_libraries[0] = vk::UniquePipeline{library_handles[0], device};
        library_link->owned_libraries[1] = vk::UniquePipeline{library_handles[3], device};
        const auto libraries_done = std::chrono::steady_clock::now();

        const vk::PipelineLibraryCreateInfoKHR link_libraries = {
            .libraryCount = static_cast<u32>(library_handles.size()),
            .pLibraries = library_handles.data(),
        };
        const vk::GraphicsPipelineCreateInfo link_info = {
            .pNext = &link_libraries,
            .flags = pipeline_info.flags,
            .layout = *pipeline_layout,
        };
        auto [link_result, linked] = CreateWithRetries("link of", debug_str, [&] {
            return device.createGraphicsPipelineUnique(pipeline_cache, link_info);
        }); // FIX-GT7-101
        ASSERT_MSG(link_result == vk::Result::eSuccess, "Failed to link graphics pipeline: {}",
                   vk::to_string(link_result));
        pipeline = std::move(linked);
        SetObjectName(device, *pipeline, "Graphics Pipeline {}", debug_str);
        const double library_ms =
            std::chrono::duration<double, std::milli>(libraries_done - create_start).count();
        if (!preloading || library_ms >= 20.0) {
            LOG_WARNING(
                Render_Vulkan,
                "Graphics pipeline {}: libraries {:.1f} ms (pre-raster {}, fragment {}), fast "
                "link {:.1f} ms; PERF-GT7-041 shared waits {}",
                debug_str,
                library_ms,
                pre_raster_reused ? "reused" : "compiled", fragment_reused ? "reused" : "compiled",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                          libraries_done)
                    .count(),
                library_cache.shared_waits.load());
        }

        if (!compile_policy.background_optimization) {
            return; // SHD-001: the fast-linked pipeline is the one drawn with
        }
        auto promise = std::make_shared<std::promise<void>>();
        library_link->optimized = promise->get_future();
        const auto layout = *pipeline_layout;
        const bool queued = PipelineLinkWorkers::Instance().TryPush(
            [this, device, layout, library_handles, promise,
             cache = pipeline_cache, debug_str, tessellated](bool stopping) {
            struct Complete {
                std::shared_ptr<std::promise<void>> promise;
                ~Complete() { promise->set_value(); }
            } complete{promise};
            if (stopping) return;
            try {
            // Optional optimization may not create an unbounded queue or compete with
            // required builds. Give up this replacement if no quiet period arrives.
            const auto idle_deadline = SteadyMs() + 5000;
            while (SteadyMs() - last_pipeline_miss_ms.load(std::memory_order_relaxed) < 3000 ||
                   g_builds_running.load(std::memory_order_relaxed) != 0) {
                if (SteadyMs() >= idle_deadline) return;
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
#ifdef _WIN32
            MEMORYSTATUSEX memory{sizeof(memory)};
            if (!GlobalMemoryStatusEx(&memory) || memory.ullAvailPageFile < (1ULL << 30) ||
                memory.ullAvailPhys < (1ULL << 30)) return;
#endif
            const vk::PipelineLibraryCreateInfoKHR libraries_info = {
                .libraryCount = static_cast<u32>(library_handles.size()),
                .pLibraries = library_handles.data(),
            };
            const vk::GraphicsPipelineCreateInfo optimized_info = {
                .pNext = &libraries_info,
                .flags = vk::PipelineCreateFlagBits::eLinkTimeOptimizationEXT,
                .layout = layout,
            };
            BuildWatch watch{debug_str, "optimized link"};
            HeavyBuildGate gate{tessellated};
            const auto link_start = std::chrono::steady_clock::now();
            auto [result, optimized] = device.createGraphicsPipeline(cache, optimized_info);
            if (result == vk::Result::eSuccess) {
                optimized_pipeline.store(optimized, std::memory_order_release);
            }
            // SHD-001: how much compiling the optimized links cost.
            const u64 us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - link_start)
                               .count();
            const u64 total_us = lto_us.fetch_add(us) + us;
            if (const u64 links = ++lto_links; links % 200 == 0) {
                LOG_WARNING(Render_Vulkan,
                            "SHD-001: {} optimized links done, {:.1f} s of driver compiling",
                            links, total_us / 1e6);
            }
            } catch (...) {
                LOG_WARNING(Render_Vulkan, "Shared pipeline {}: optional optimization failed; keeping fast link", debug_str);
            }
        });
        if (!queued) promise->set_value();
        return;
    }

    HeavyBuildGate whole_gate{tessellated};
    std::optional<BuildWatch> whole_watch;
    if (!preloading) {
        whole_watch.emplace(debug_str, "whole pipeline compile");
    }
    auto [pipeline_result, pipe] = CreateWithRetries("pipeline", debug_str, [&] {
        return device.createGraphicsPipelineUnique(pipeline_cache, pipeline_info);
    }); // FIX-GT7-101
    whole_watch.reset();
    // PERF-DIAG-013: driver compile time of graphics pipelines created at runtime.
    if (!preloading) {
        LOG_WARNING(Render_Vulkan, "Graphics pipeline {}: driver create {:.1f} ms", debug_str,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              create_start)
                        .count());
    }
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Graphics Pipeline {}", debug_str);
}

bool GraphicsPipeline::CreateShaderObjects(
    std::span<const vk::ShaderModule> modules, const SerializationSupport& sdata,
    std::span<const vk::PipelineColorBlendAttachmentState> attachments, bool fragment,
    bool independent_sets) {
    // SO-001/SO-003: the stages in draw order; the rect/quad list emulation's control and
    // evaluation shaders and the clip distance emulation's discard shader are generated code.
    const bool tessellation = so_state.patch_control_points != 0;
    const auto code_of = [&](Shader::SwStage stage, const std::vector<u32>& generated) {
        const auto module = modules[u32(stage)];
        return module ? ModuleCode(instance.GetDevice(), module) : generated;
    };
    std::array<std::vector<u32>, 4> code;
    code[0] = ModuleCode(instance.GetDevice(), modules[u32(Shader::SwStage::Vertex)]);
    if (tessellation) {
        code[1] = code_of(Shader::SwStage::TessellationControl, sdata.tcs);
        code[2] = code_of(Shader::SwStage::TessellationEval, sdata.tes);
    }
    if (fragment) {
        code[3] = ModuleCode(instance.GetDevice(), modules[u32(Shader::SwStage::Fragment)]);
    } else if (!sdata.fragment.empty()) {
        code[3] = sdata.fragment; // clip distance emulation's discard shader
    }
    if (code[0].empty() || (tessellation && (code[1].empty() || code[2].empty())) ||
        ((fragment || !sdata.fragment.empty()) && code[3].empty())) {
        static std::atomic<u32> logged{};
        if (++logged <= 10) {
            LOG_WARNING(Render_Vulkan,
                        "SO-004: no kept code: vertex {} control {} evaluation {} fragment {}",
                        code[0].size(), code[1].size(), code[2].size(), code[3].size());
        }
        return false; // a module's code wasn't kept: a pipeline as before
    }
    const vk::PushConstantRange push_constants = {
        .stageFlags = AllGraphicsStageBits,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };
    // AMD-301: the subgroup size the pipeline stages would have required.
    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size = {
        .requiredSubgroupSize = 64,
    };
    const vk::ShaderStageFlags sized_stages = instance.RequiredSubgroupSizeStages();
    constexpr std::array<vk::ShaderStageFlagBits, 4> stage_bits{
        vk::ShaderStageFlagBits::eVertex, vk::ShaderStageFlagBits::eTessellationControl,
        vk::ShaderStageFlagBits::eTessellationEvaluation, vk::ShaderStageFlagBits::eFragment};
    // SO-007: the stages are made together, linked (RYGT_SO_LINK=0: each on its own), so the
    // driver sees the whole set as it does a pipeline's: it can merge the stages the GPU runs as
    // one program and drop what the next stage never reads. SO-006: for independent-sets layouts.
    static const bool link = [] {
        const char* env = std::getenv("RYGT_SO_LINK");
        return !(env && env[0] == '0');
    }();
    vk::ShaderCreateFlagsEXT flags{};
    if (link) {
        flags |= vk::ShaderCreateFlagBitsEXT::eLinkStage;
    }
    if (independent_sets) {
        flags |= vk::ShaderCreateFlagsEXT{static_cast<VkShaderCreateFlagsEXT>(0x00040000)};
    }
    // SO-008: shader objects are shared by every pipeline with the same stage code and set
    // layouts (pipelines that differ only in blending, formats or other drawing state).
    u64 share_key = XXH3_64bits(&flags, sizeof(flags)) ^ num_sets;
    for (u32 i = 0; i < 4; ++i) {
        share_key = XXH3_64bits_withSeed(code[i].data(), code[i].size() * sizeof(u32),
                                         share_key + i);
    }
    share_key = XXH3_64bits_withSeed(set_layout_hashes.data(),
                                     set_layout_hashes.size() * sizeof(set_layout_hashes[0]),
                                     share_key);
    struct ShaderObjectBuildFailed {};
    auto& shared_sets = instance.ShaderObjectBuilds();
#ifndef NDEBUG
    static std::atomic<u64> shared_hits{};
#endif
    bool reused = false;
    try {
        auto result = shared_sets.GetOrCreate(share_key, true, [&] {
            boost::container::static_vector<vk::ShaderCreateInfoEXT, 4> create_infos;
            std::array<u32, 4> slot{};
            for (u32 i = 0; i < 4; ++i) {
                if (code[i].empty()) {
                    continue;
                }
                vk::ShaderStageFlags next{};
                for (u32 j = i + 1; j < 4; ++j) {
                    if (!code[j].empty()) {
                        next = stage_bits[j];
                        break;
                    }
                }
                if (i == 0 && !tessellation) {
                    next = vk::ShaderStageFlagBits::eFragment;
                }
                slot[i] = static_cast<u32>(create_infos.size());
                create_infos.push_back(vk::ShaderCreateInfoEXT{
                    .pNext = instance.IsSubgroupSize64Supported() && (sized_stages & stage_bits[i])
                                 ? &subgroup_size
                                 : nullptr,
                    .flags = flags,
                    .stage = stage_bits[i],
                    .nextStage = next,
                    .codeType = vk::ShaderCodeTypeEXT::eSpirv,
                    .codeSize = code[i].size() * sizeof(u32),
                    .pCode = code[i].data(),
                    .pName = "main",
                    .setLayoutCount = num_sets,
                    .pSetLayouts = set_layouts.data(),
                    .pushConstantRangeCount = 1,
                    .pPushConstantRanges = &push_constants,
                });
            }
            std::array<VkShaderEXT, 4> created{};
            struct CreatedShadersGuard {
                VkDevice device;
                std::array<VkShaderEXT, 4>& shaders;
                ~CreatedShadersGuard() {
                    for (const auto shader : shaders)
                        if (shader) VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyShaderEXT(device, shader, nullptr);
                }
            } created_guard{instance.GetDevice(), created};
#ifndef NDEBUG
            const auto create_start = std::chrono::steady_clock::now();
#endif
            // SO-009: the driver's compiled shader objects are kept on disk (shader_objects in the cache
            // folder, one file a set, keyed by the stage code, layouts, flags, GPU and driver version)
            // and made from those binaries on later starts: the driver compiles a set only once.
            // RYGT_SO_CACHE=0: always from SPIR-V.
            static const bool use_cache = [] {
                const char* env = std::getenv("RYGT_SO_CACHE");
                return !(env && env[0] == '0');
            }();
            const auto& binary_properties = instance.ShaderObjectProperties();
            const u64 binary_key = XXH3_64bits_withSeed(binary_properties.shaderBinaryUUID.data(),
                binary_properties.shaderBinaryUUID.size(),
                share_key ^ u64(binary_properties.shaderBinaryVersion));
            const auto cache_file =
                Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / "shader_objects" /
                fmt::format("{:016x}.bin", binary_key ^ (u64(instance.GetDriverVersion()) << 32) ^
                                               instance.GetDeviceID());
            // SO-013: format 2 adds a checksum of each binary: a damaged file is never handed to the
            // driver (compiled again and rewritten instead).
            constexpr u32 CacheMagic = ShaderObjectCacheMagic;
            bool from_binary = false;
            VkResult result = VK_ERROR_UNKNOWN;
            if (use_cache) {
                std::vector<std::vector<u8>> binaries;
                if (std::FILE* file = std::fopen(cache_file.string().c_str(), "rb")) {
                    binaries = ReadShaderObjectBinaries(file, static_cast<u32>(create_infos.size()));
                    std::fclose(file);
                }
                if (!binaries.empty()) {
                    auto binary_infos = create_infos;
                    std::vector<std::vector<ShaderBinaryBlock>> aligned_binaries(binary_infos.size());
                    for (size_t i = 0; i < binary_infos.size(); ++i) {
                        aligned_binaries[i] = AlignShaderBinary(binaries[i]);
                        binary_infos[i].codeType = vk::ShaderCodeTypeEXT::eBinary;
                        binary_infos[i].codeSize = binaries[i].size();
                        binary_infos[i].pCode = aligned_binaries[i].data();
                    }
                    result = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateShadersEXT(
                        instance.GetDevice(), static_cast<u32>(binary_infos.size()),
                        reinterpret_cast<const VkShaderCreateInfoEXT*>(binary_infos.data()), nullptr,
                        created.data());
                    from_binary = result == VK_SUCCESS;
                    if (!from_binary) {
                        for (VkShaderEXT& shader : created) {
                            if (shader) {
                                VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyShaderEXT(instance.GetDevice(),
                                                                                 shader, nullptr);
                                shader = VK_NULL_HANDLE;
                            }
                        }
                    }
                }
            }
            if (!from_binary) {
                // SO-014: AMD's driver compiles a shader object set for up to seconds of CPU time; at
                // normal priority those compiles competed with the game's threads (game-side stalls in
                // races). Below normal while it runs. RYGT_SO_COMPILE_PRIORITY=0: unchanged.
                static const bool lower = [] {
                    const char* env = std::getenv("RYGT_SO_COMPILE_PRIORITY");
                    return !(env && env[0] == '0');
                }();
#ifdef _WIN32
                const HANDLE thread = GetCurrentThread();
                const int priority = GetThreadPriority(thread);
                const int applied = std::min(priority, int(THREAD_PRIORITY_BELOW_NORMAL));
                const bool owns_priority = lower && priority != THREAD_PRIORITY_ERROR_RETURN &&
                                           SetThreadPriority(thread, applied);
#endif
                result = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateShadersEXT(
                    instance.GetDevice(), static_cast<u32>(create_infos.size()),
                    reinterpret_cast<const VkShaderCreateInfoEXT*>(create_infos.data()), nullptr,
                    created.data());
#ifdef _WIN32
                if (owns_priority && GetThreadPriority(thread) == applied) {
                    SetThreadPriority(thread, priority);
                }
#endif
                if (result == VK_SUCCESS && use_cache) {
                    // Written beside and renamed, so a reader never sees half a file.
                    std::error_code ec;
                    std::filesystem::create_directories(cache_file.parent_path(), ec);
                    static std::atomic<u64> temp_sequence{};
                    auto temporary = cache_file;
#ifdef _WIN32
                    const u64 process = GetCurrentProcessId();
#else
                    const u64 process = static_cast<u64>(getpid());
#endif
                    temporary += fmt::format(".{}.{}.tmp", process, temp_sequence.fetch_add(1));
                    bool ok = false;
                    struct TempCleanup {
                        const std::filesystem::path& path;
                        bool owned{};
                        ~TempCleanup() { if (owned) { std::error_code ignored; std::filesystem::remove(path, ignored); } }
                    } temp_cleanup{temporary};
                    using FileOwner = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
                    FileOwner output{std::fopen(temporary.string().c_str(), "wbx"), &std::fclose};
                    if (std::FILE* file = output.get()) {
                        temp_cleanup.owned = true;
                        const u32 count = static_cast<u32>(create_infos.size());
                        ok = std::fwrite(&CacheMagic, 4, 1, file) == 1 &&
                             std::fwrite(&count, 4, 1, file) == 1;
                        for (u32 i = 0; ok && i < count; ++i) {
                            size_t size = 0;
                            ok = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetShaderBinaryDataEXT(
                                     instance.GetDevice(), created[i], &size, nullptr) == VK_SUCCESS &&
                                 size != 0;
                            std::vector<u8> data(size);
                            ok = ok && VULKAN_HPP_DEFAULT_DISPATCHER.vkGetShaderBinaryDataEXT(
                                           instance.GetDevice(), created[i], &size, data.data()) ==
                                           VK_SUCCESS;
                            const u64 size64 = size;
                            const u64 checksum = XXH3_64bits(data.data(), size);
                            ok = ok && std::fwrite(&size64, 8, 1, file) == 1 &&
                                 std::fwrite(&checksum, 8, 1, file) == 1 &&
                                 std::fwrite(data.data(), 1, size, file) == size;
                        }
                        ok = std::fclose(output.release()) == 0 && ok;
                    }
                    if (ok) {
#ifdef _WIN32
                        if (!MoveFileExW(temporary.c_str(), cache_file.c_str(), MOVEFILE_WRITE_THROUGH))
                            ec = std::error_code(GetLastError(), std::system_category());
#else
                        std::filesystem::create_hard_link(temporary, cache_file, ec);
                        if (!ec) std::filesystem::remove(temporary, ec);
#endif
                    }
                }
            }
#ifndef NDEBUG
            static std::atomic<u32> binary_hits{};
            static std::atomic<u32> compiled{};
            (from_binary ? binary_hits : compiled).fetch_add(1, std::memory_order_relaxed);
            // SO-005 (diagnosis): how long the driver takes to make shader objects, per pipeline.
            {
                const double ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - create_start)
                                      .count();
                static std::mutex timing_mutex;
                static u32 timed = 0;
                static double total_ms = 0, max_ms = 0;
                std::scoped_lock lock{timing_mutex};
                ++timed;
                total_ms += ms;
                max_ms = std::max(max_ms, ms);
                if (timed <= 5 || timed % 50 == 0) {
                    LOG_WARNING(Render_Vulkan,
                                "SO-005: shader objects of {} stages made in {:.1f} ms ({}; SO-009 cache: "
                                "{} from disk, {} compiled); {} pipelines so far, "
                                "{:.1f} ms average, {:.1f} ms longest",
                                create_infos.size(), ms, from_binary ? "from disk" : "compiled",
                                binary_hits.load(), compiled.load(), timed, total_ms / timed, max_ms);
                }
            }
#endif
            // PERF-GT7-108: a set the driver compiled from SPIR-V, kept as a compile pack recipe.
            if (result == VK_SUCCESS && !from_binary && use_cache && CompilePack::RecordingOn()) {
                static const auto pack_dir = CompilePack::Dir(
                    Common::FS::GetUserPath(Common::FS::PathType::CacheDir),
                    Common::Singleton<Common::ElfInfo>::Instance()->GameSerial(),
                    instance.GetVendorID(), instance.GetDeviceID());
                const auto pack_file = CompilePack::File(pack_dir, share_key);
                std::error_code pack_ec;
                if (!std::filesystem::exists(pack_file, pack_ec)) {
                    CompilePack::Recipe recipe{
                        .key = share_key,
                        .flags = static_cast<u32>(static_cast<VkShaderCreateFlagsEXT>(flags)),
                        .push_stages =
                            static_cast<u32>(static_cast<VkShaderStageFlags>(push_constants.stageFlags)),
                        .push_size = push_constants.size,
                    };
                    bool recordable = true;
                    recipe.sets.resize(num_sets);
                    for (u32 set = 0; set < num_sets; ++set) {
                        recipe.sets[set].flags =
                            set == push_set
                                ? static_cast<u32>(VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR)
                                : 0u;
                        for (const auto& binding : pack_bindings[set]) {
                            recordable = recordable && binding.pImmutableSamplers == nullptr;
                            recipe.sets[set].bindings.push_back(
                                {binding.binding, static_cast<u32>(binding.descriptorType),
                                 binding.descriptorCount,
                                 static_cast<u32>(static_cast<VkShaderStageFlags>(binding.stageFlags))});
                        }
                    }
                    for (const auto& info : create_infos) {
                        auto& stage = recipe.stages.emplace_back();
                        stage.stage = static_cast<u32>(info.stage);
                        stage.next = static_cast<u32>(static_cast<VkShaderStageFlags>(info.nextStage));
                        const auto* words = static_cast<const u32*>(info.pCode);
                        stage.code.assign(words, words + info.codeSize / sizeof(u32));
                    }
                    static std::atomic<u32> recorded{};
                    if (recordable && CompilePack::Write(pack_file, recipe)) {
                        const u32 n = ++recorded;
                        if (n == 1 || n % 500 == 0) {
                            LOG_INFO(Render_Vulkan, "PERF-GT7-108: {} compile pack recipes kept ({})",
                                     n, pack_dir.string());
                        }
                    }
                }
            }
            if (result != VK_SUCCESS) {
                static std::atomic<u32> logged{};
                if (++logged <= 8) {
                    LOG_WARNING(Render_Vulkan, "SO-001: shader objects failed ({}); a pipeline instead",
                                vk::to_string(vk::Result(result)));
                }
                throw ShaderObjectBuildFailed{};
            }
            auto objects = std::make_shared<SharedShaderObjects>();
            objects->device = instance.GetDevice();
            for (u32 i = 0; i < 4; ++i) {
                if (!code[i].empty()) {
                    objects->objects[i] = vk::ShaderEXT{created[slot[i]]};
                    created[slot[i]] = VK_NULL_HANDLE;
                }
            }
            return objects;
        });
        shader_objects = result.first->objects;
        reused = result.second;
    } catch (const ShaderObjectBuildFailed&) {
        return false;
    }
#ifndef NDEBUG
    if (reused) {
        const u64 hits = ++shared_hits;
        if (hits <= 3 || hits % 200 == 0) {
            LOG_WARNING(Render_Vulkan, "SO-008: {} reuses shared shader objects ({} reuses)",
                        GetDebugString(), hits);
        }
    }
#endif
    for (u32 i = 0; i < so_state.attachments; ++i) {
        const auto& attachment = attachments[i];
        so_state.blend_enable[i] = attachment.blendEnable;
        so_state.equations[i] = vk::ColorBlendEquationEXT{
            .srcColorBlendFactor = attachment.srcColorBlendFactor,
            .dstColorBlendFactor = attachment.dstColorBlendFactor,
            .colorBlendOp = attachment.colorBlendOp,
            .srcAlphaBlendFactor = attachment.srcAlphaBlendFactor,
            .dstAlphaBlendFactor = attachment.dstAlphaBlendFactor,
            .alphaBlendOp = attachment.alphaBlendOp,
        };
    }
    if (reused) {
        return true;
    }
#ifndef NDEBUG
    static std::atomic<u32> made{};
    static std::atomic<u32> made_tessellated{};
    const u32 n = ++made;
    const u32 t = tessellation ? ++made_tessellated : made_tessellated.load();
    if (n <= 3 || n % 500 == 0 || (tessellation && (t <= 3 || t % 100 == 0))) {
        LOG_WARNING(Render_Vulkan,
                    "SO-001: {} drawn with shader objects ({} so far, {} tessellated, SO-003)",
                    GetDebugString(), n, t);
    }
#endif
    return true;
}

void GraphicsPipeline::BindShaderObjects(
    vk::CommandBuffer cmd, const ShaderObjectState* previous,
    const std::array<vk::ShaderEXT, 4>* previous_shaders) const {
    if (!previous_shaders || *previous_shaders != shader_objects) {
        std::array<vk::ShaderStageFlagBits, 5> stages{vk::ShaderStageFlagBits::eVertex,
                                                      vk::ShaderStageFlagBits::eFragment};
        std::array<vk::ShaderEXT, 5> shaders{shader_objects[0], shader_objects[3]};
        u32 count = 2;
        if (instance.IsTessellationShaderEnabled()) {
            stages[count] = vk::ShaderStageFlagBits::eTessellationControl;
            shaders[count++] = shader_objects[1];
            stages[count] = vk::ShaderStageFlagBits::eTessellationEvaluation;
            shaders[count++] = shader_objects[2];
        }
        if (instance.IsGeometryShaderEnabled()) {
            stages[count++] = vk::ShaderStageFlagBits::eGeometry;
        }
        cmd.bindShadersEXT(count, stages.data(), shaders.data());
    }
    const auto& s = so_state;
    const ShaderObjectState* p = previous;
    // SO-010: each command only when its value differs from the previous draw's.
    if (!p || p->topology != s.topology) {
        cmd.setPrimitiveTopology(s.topology);
    }
    if (s.patch_control_points != 0 &&
        (!p || p->patch_control_points != s.patch_control_points)) {
        // SO-003: as the pipeline's tessellation state (no domain origin state: upper left).
        cmd.setPatchControlPointsEXT(s.patch_control_points);
        cmd.setTessellationDomainOriginEXT(vk::TessellationDomainOrigin::eUpperLeft);
    }
    if (!p || p->polygon_mode != s.polygon_mode) {
        cmd.setPolygonModeEXT(s.polygon_mode);
    }
    if (!p || p->samples != s.samples) {
        cmd.setRasterizationSamplesEXT(s.samples);
        const vk::SampleMask mask = ~vk::SampleMask{0};
        cmd.setSampleMaskEXT(s.samples, &mask);
    }
    if (!p || p->alpha_to_coverage != s.alpha_to_coverage) {
        cmd.setAlphaToCoverageEnableEXT(s.alpha_to_coverage);
    }
    if (!p || p->depth_clamp != s.depth_clamp) {
        cmd.setDepthClampEnableEXT(s.depth_clamp);
    }
    if (!p || p->depth_clip != s.depth_clip) {
        cmd.setDepthClipEnableEXT(s.depth_clip);
    }
    if (!p || p->negative_one_to_one != s.negative_one_to_one) {
        cmd.setDepthClipNegativeOneToOneEXT(s.negative_one_to_one);
    }
    if (!p || p->provoking != s.provoking) {
        cmd.setProvokingVertexModeEXT(s.provoking);
    }
    // FIX-GTS-425: with VK_NV_framebuffer_mixed_samples enabled, a shader-object draw must set
    // the coverage modulation mode (VUID-vkCmdDrawIndexed-None-07644); it was never set, so it
    // was undefined on every shader-object draw. Pipelines leave it at NONE.
    if (!p && instance.IsNvFramebufferMixedSamples()) {
        cmd.setCoverageModulationModeNV(vk::CoverageModulationModeNV::eNone);
    }
    if (instance.IsLogicOpSupported() &&
        (!p || p->logic_op_enable != s.logic_op_enable || p->logic_op != s.logic_op)) {
        cmd.setLogicOpEnableEXT(s.logic_op_enable);
        cmd.setLogicOpEXT(s.logic_op);
    }
    if (s.attachments != 0) {
        const auto same = [&](const auto& a, const auto& b) {
            return std::equal(a.begin(), a.begin() + s.attachments, b.begin());
        };
        // DIAG-GTS-448: with RYGT_SO_TOGGLE_STATE=1, a bind after a pipeline first sets the
        // opposite blend enables on all 8 targets (and real equations on all 8), then the real ones.
        static const bool toggle_state = [] {
            const char* env = std::getenv("RYGT_SO_TOGGLE_STATE");
            return env && env[0] == '1';
        }();
        if (!p && toggle_state) {
            std::array<vk::Bool32, AmdGpu::NUM_COLOR_BUFFERS> flipped{};
            for (u32 i = 0; i < flipped.size(); ++i) {
                flipped[i] = s.blend_enable[i] ? VK_FALSE : VK_TRUE;
            }
            cmd.setColorBlendEnableEXT(0u, vk::ArrayProxy<const vk::Bool32>{flipped});
            cmd.setColorBlendEquationEXT(
                0u, vk::ArrayProxy<const vk::ColorBlendEquationEXT>{s.equations});
            cmd.setColorBlendEnableEXT(0u, vk::ArrayProxy<const vk::Bool32>{s.blend_enable});
        }
        if (!p || p->attachments < s.attachments || !same(p->blend_enable, s.blend_enable)) {
            cmd.setColorBlendEnableEXT(0, s.attachments, s.blend_enable.data());
        }
        if (!p || p->attachments < s.attachments || !same(p->equations, s.equations)) {
            cmd.setColorBlendEquationEXT(0, s.attachments, s.equations.data());
        }
    }
}

GraphicsPipeline::~GraphicsPipeline() {
    // SO-008: shader objects are shared and live as long as the renderer.
    if (library_link && library_link->optimized.valid()) {
        library_link->optimized.wait();
    }
    if (const VkPipeline optimized = optimized_pipeline.exchange(VK_NULL_HANDLE)) {
        instance.GetDevice().destroyPipeline(optimized);
    }
}

template <typename Attribute, typename Binding>
void GraphicsPipeline::GetVertexInputs(
    VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1,
    const Shader::Info& vs_info) const {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    if (fetch_shader.Empty() || fetch_shader.attributes.empty()) {
        return;
    }
    for (u32 semantic = 0; semantic < fetch_shader.attributes.size(); ++semantic) {
        const auto& attrib = fetch_shader.attributes[semantic];
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = attrib.GetSharp(vs_info);
        auto format = LiverpoolToVK::TrySurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        if (format == vk::Format::eUndefined) {
            // FIX-052: a garbage V# (data format 13 with number format 1 in a GT Sport race)
            // asserted and stopped the game. Its attribute reads four floats instead; the draw's
            // data is garbage either way.
            static std::atomic<u32> logged{};
            if (logged++ < 10) {
                LOG_WARNING(Render_Vulkan,
                            "FIX-052: vertex attribute {} has no format (data {}, number {})",
                            semantic, static_cast<u32>(buffer.GetDataFmt()),
                            static_cast<u32>(buffer.GetNumberFmt()));
            }
            format = vk::Format::eR32G32B32A32Sfloat;
        }
        attributes.push_back(Attribute{
            .location = semantic,
            .binding = semantic,
            .format = format,
            .offset = 0,
        });
        // DIAG-GTS-427: a guest stride (up to 16383) above the device's limit (2048 on NVIDIA)
        // makes the vertex input undefined; validation saw 16023 once in a GT Sport race.
        if (const u32 stride = buffer.GetStride();
            stride > instance.GetImageLimits().maxVertexInputBindingStride) {
            static std::atomic<u32> logged{};
            if (const u32 n = ++logged; n <= 20 || n % 1000 == 0) {
                LOG_WARNING(Render_Vulkan,
                            "DIAG-GTS-427: vs {:#x} attribute {} stride {} > limit {} (base {:#x}, "
                            "step {}, {} times)",
                            vs_info.pgm_hash, semantic, stride,
                            instance.GetImageLimits().maxVertexInputBindingStride,
                            u64(buffer.base_address), u32(step_rate), n);
            }
        }
        // FIX-GTS-430: a stride above the limit is undefined behaviour; a GT Sport race passed
        // 16256 for an attribute with no buffer (base 0) 16 times right before a device loss.
        // Such a binding gets stride 0 (valid: every vertex reads the same element).
        const u32 bound_stride =
            buffer.GetStride() > instance.GetImageLimits().maxVertexInputBindingStride
                ? 0u
                : buffer.GetStride();
        bindings.push_back(Binding{
            .binding = semantic,
            .stride = bound_stride,
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
        });
        const u32 divisor = step_rate == InstanceIdType::OverStepRate0
                                ? step_rate_0
                                : (step_rate == InstanceIdType::OverStepRate1 ? step_rate_1 : 1);
        if constexpr (std::is_same_v<Binding, vk::VertexInputBindingDescription2EXT>) {
            bindings.back().divisor = divisor;
        } else if (step_rate != InstanceIdType::None) {
            divisors.push_back(vk::VertexInputBindingDivisorDescriptionEXT{
                .binding = semantic,
                .divisor = divisor,
            });
        }
        guest_buffers.emplace_back(buffer);
    }
}

// Declare templated GetVertexInputs for necessary types.
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription>& attributes,
    VertexInputs<vk::VertexInputBindingDescription>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1,
    const Shader::Info& vs_info) const;
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription2EXT>& attributes,
    VertexInputs<vk::VertexInputBindingDescription2EXT>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1,
    const Shader::Info& vs_info) const;

void GraphicsPipeline::BuildDescSetLayout(bool preloading) {
    // SHD-002: one set per stage, its bindings numbered from 0.
    std::array<boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32>,
               Shader::Backend::MaxDescriptorSets>
        set_bindings;
    std::array<u32, Shader::Backend::MaxDescriptorSets> set_descriptors{};

    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const u32 set = Shader::Backend::StageSet(u32(stage->sw_stage));
        auto& bindings = set_bindings[set];
        u32 binding{};
        if (const u32 sw = u32(stage->sw_stage);
            sw < Shader::Backend::StageBufferOffsetCount.size() &&
            stage->buffers.size() > Shader::Backend::StageBufferOffsetCount[sw]) {
            static std::atomic<u32> logged{};
            if (logged++ < 10) {
                LOG_ERROR(Render_Vulkan,
                          "SHD-002: {}_{:#x} has {} buffers, more than its {} push offsets",
                          stage->hw_stage, stage->pgm_hash, stage->buffers.size(),
                          Shader::Backend::StageBufferOffsetCount[sw]);
            }
        }
        const auto stage_bit = LogicalStageToStageBit[u32(stage->sw_stage)];
        for (const auto& buffer : stage->buffers) {
            if (!preloading) {
                [[maybe_unused]] const auto sharp = buffer.GetSharp(*stage);
            }
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
        for (const auto& image : stage->images) {
            const u32 num_bindings = image.NumBindings(*stage);
            bindings.push_back({
                .binding = binding,
                .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                .descriptorCount = num_bindings,
                .stageFlags = stage_bit,
            });
            binding += num_bindings;
        }
        for (const auto& sampler : stage->samplers) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eSampler,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
        set_descriptors[set] = binding;
    }
    // Repeated material pushes dominate NVIDIA command recording. Fragment sets
    // can instead use the Native recorder's immutable, full-key descriptor cache.
    // Vertex pushes remain available; other backends retain their previous policy.
    bool cached_fragment = false;
#ifdef RYTY_GPU
    static const char* const descriptor_override = std::getenv("RYGT_NVIDIA_CACHED_FRAGMENT_SETS");
    static const char* const amd_descriptor_override = std::getenv("RYGT_AMD_CACHED_FRAGMENT_SETS");
    cached_fragment = CacheFragmentDescriptors(instance.GetVendorID(), NativeModeRequested(),
                                                descriptor_override, amd_descriptor_override);
    if (cached_fragment) {
        static std::once_flag logged;
        std::call_once(logged, [vendor = instance.GetVendorID()] {
            LOG_INFO(Render_Vulkan, "DESC-POLICY: {} fragment sets use immutable descriptor caching; vertex push descriptors retained when supported",
                     vendor == 0x1002 ? "AMD (AMD-PERF-002)" : "NVIDIA");
        });
    }
#endif
    push_set = NoPushSet;
    // DIAG-GTS-426: RYGT_PUSH_DESCRIPTORS=0 gives graphics pipelines no push set (every stage
    // binds allocated sets), to test the push path and to get validation past its push crash.
    static const bool push_descriptors = [] {
        const char* env = std::getenv("RYGT_PUSH_DESCRIPTORS");
        return !(env && env[0] == '0');
    }();
    for (const auto sw : {Shader::SwStage::Fragment, Shader::SwStage::Vertex}) {
        if (!push_descriptors) break;
        if (cached_fragment && sw == Shader::SwStage::Fragment) continue;
        const u32 set = Shader::Backend::StageSet(u32(sw));
        if (!set_bindings[set].empty() && set_descriptors[set] < instance.MaxPushDescriptors()) {
            push_set = set;
            break;
        }
    }
    // PERF-GTS-374 (RYGT_DYNAMIC_BUFFER_OFFSETS=1, native mode): the sets that are not pushed
    // take their buffers with dynamic offsets. A race draw's buffers sit at new offsets each
    // time (its constants are copied to fresh upload memory), so with the offset in the
    // descriptor nearly every draw made and wrote new sets (5% reused); with it given at the
    // bind, 42% match a set of the same list and 93% one of the last 10 s (DIAG-GTS-373).
    // All or nothing for a pipeline: the device limits dynamic storage buffers per layout.
    set_dynamic_buffers = {};
#ifdef RYTY_GPU
    static const bool dynamic_requested = [] {
        const char* env = std::getenv("RYGT_DYNAMIC_BUFFER_OFFSETS");
        // PERF-GTS-407: on unless set to something else
        return (!env || !*env || env[0] == '1') && NativeModeRequested();
    }();
    if (dynamic_requested) {
        static const u32 dynamic_limit = instance.GetPhysicalDevice()
                                             .getProperties()
                                             .limits.maxDescriptorSetStorageBuffersDynamic;
        std::array<u32, Shader::Backend::MaxDescriptorSets> counts{};
        u32 total = 0;
        for (u32 set = 0; set < set_bindings.size(); ++set) {
            if (set == push_set) continue;
            for (const auto& binding : set_bindings[set]) {
                counts[set] += binding.descriptorType == vk::DescriptorType::eStorageBuffer;
            }
            total += counts[set];
        }
        if (total != 0 && total <= dynamic_limit) {
            for (u32 set = 0; set < set_bindings.size(); ++set) {
                if (counts[set] == 0 || counts[set] > 255) continue;
                for (auto& binding : set_bindings[set]) {
                    if (binding.descriptorType == vk::DescriptorType::eStorageBuffer) {
                        binding.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
                    }
                }
                set_dynamic_buffers[set] = static_cast<u8>(counts[set]);
            }
        }
    }
#endif
    const vk::Device device = instance.GetDevice();
    num_sets = 0;
    set_layout_hashes = {};
    if (CompilePack::RecordingOn()) { // PERF-GT7-108
        for (u32 set = 0; set < set_bindings.size(); ++set) {
            pack_bindings[set].assign(set_bindings[set].begin(), set_bindings[set].end());
        }
    }
    for (u32 set = 0; set < set_bindings.size(); ++set) {
        auto& bindings = set_bindings[set];
        if (bindings.empty()) {
            continue;
        }
        const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
            .flags = set == push_set ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                                     : vk::DescriptorSetLayoutCreateFlagBits{},
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        };
        set_layout_hashes[set] =
            XXH3_64bits(bindings.data(), bindings.size() * sizeof(bindings[0])) ^
            (set == push_set ? 0x9E3779B97F4A7C15ULL : 0) ^ (u64(set) << 56) | 1;
        auto [layout_result, layout] = device.createDescriptorSetLayoutUnique(desc_layout_ci);
        ASSERT_MSG(layout_result == vk::Result::eSuccess,
                   "Failed to create graphics descriptor set layout: {}",
                   vk::to_string(layout_result));
        desc_layouts[set] = std::move(layout);
        set_layouts[set] = *desc_layouts[set];
        num_sets = set + 1;
    }
    // Sets between stage sets: an empty layout.
    for (u32 set = 0; set < num_sets; ++set) {
        if (!set_layouts[set]) {
            if (!empty_set_layout) {
                auto [result, layout] =
                    device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo{});
                ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create an empty layout: {}",
                           vk::to_string(result));
                empty_set_layout = std::move(layout);
            }
            set_layouts[set] = *empty_set_layout;
        }
    }
}

} // namespace Vulkan
