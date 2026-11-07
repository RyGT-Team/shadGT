#include "video_core/renderer_vulkan/shared_pipeline_policy.h"
#include "common/geometry_trace.h"
#include "video_core/renderer_vulkan/oneshot_shader.h"
#include "video_core/renderer_vulkan/ordered_parallel_read.h"
#include "video_core/renderer_vulkan/bounded_read_cache.h"
#include "video_core/renderer_vulkan/driver_cache_snapshot.h"
#include "video_core/renderer_vulkan/pipeline_retention_budget.h"
#include "video_core/renderer_vulkan/pipeline_cleanup_worker.h"
#include "video_core/renderer_vulkan/draw_preparation_options.h"
#include "video_core/renderer_vulkan/compile_cpu_batch.h"
#include "video_core/renderer_vulkan/stale_pipeline_builds.h"
#include "video_core/renderer_vulkan/read_ahead_budget.h"
#include "video_core/renderer_vulkan/speculative_key.h"
#include "shader_recompiler/lane_read_policy.h"
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <array>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <cstdlib>
#include "video_core/renderer_vulkan/tess_lds_scratch.h"
#include <thread>
#include <string>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#endif

#include "common/diagnostics.h"
#include "common/elf_info.h"
#include "common/guest_clock.h"
#include "common/nvtx.h"
#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/perf_monitor.h"
#include "common/recoverable.h"
#ifdef RYTY_GPU
#include "ryty_gpu/native/menu_compile_policy.h"
#include "ryty_gpu/ryty_hdr_ui.h"
#include "ryty_gpu/ryty_ghost.h"
#include "video_core/renderer_vulkan/vk_rasterizer_native.h"
#endif
#include "common/read_capture.h"
#include "common/scope_exit.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#ifndef NDEBUG
#include "shader_recompiler/srt_arena_trace.h"
#endif
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/buffer_cache/direct_memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/build_queue.h"
#include "video_core/renderer_vulkan/compiler_worker_budget.h"
#include "video_core/renderer_vulkan/translation_priority.h"
#include "video_core/renderer_vulkan/translation_retry.h"
#include "video_core/renderer_vulkan/single_sample_barycentric.h"
#include "video_core/renderer_vulkan/translation_job_match.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/spirv_check.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/shader_usage_serde.h"
#include "video_core/renderer_vulkan/required_pipeline_wait.h"
#include "video_core/renderer_vulkan/one_off_regs.h"
#include "video_core/renderer_vulkan/retry_input_policy.h"
#include "video_core/renderer_vulkan/read_ahead_retry_inputs.h"
#include "video_core/renderer_vulkan/exit_save_registry.h"
#include "video_core/renderer_vulkan/compute_warm_recipe.h"
#include "video_core/renderer_vulkan/shader_store_readers.h"
#include "video_core/shader_store_policy.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "ryty_gpu/ryty_diag.h"

namespace Shader::Optimization {
extern thread_local std::span<const u32> g_diag_compiling_code;
extern thread_local u64 g_diag_compiling_hash;
} // namespace Shader::Optimization
namespace Vulkan {
u32 TessLdsEmulation(const AmdGpu::Regs& regs) {
    static const u32 mode = [] {
        const char* env = std::getenv("RYGT_TESS_LDS_EMULATION");
        return env && *env ? u32(env[0] - '0') : 2u;
    }();
    if (mode == 0 || !regs.stage_enable.hs_en || regs.ls_hs_config.hs_input_control_points == 0 ||
        regs.ls_hs_config.num_patches <= 1) return 0;
    return TessLdsModeForMask(mode, regs.reg_array[0xa30e], regs.reg_array[0xa30f]);
}
namespace {
ExitSaveRegistry<PipelineCache>& ExitCacheRegistry() {
    static auto* registry = new ExitSaveRegistry<PipelineCache>();
    return *registry;
}
}
bool SaveDriverCacheAtExit() {
    return ExitCacheRegistry().Invoke([](PipelineCache& cache) { cache.PersistDriverCacheForExit(); });
}
static PipelineCleanupWorker& PipelineRetirementWorker() {
    static PipelineCleanupWorker worker([] {
        Common::SetCurrentThreadName("shadGT:PipelineCleanup");
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        SetThreadPriorityBoost(GetCurrentThread(), TRUE);
#endif
    });
    return worker;
}


namespace {
std::atomic<u32> g_shader_translations_running{};
struct TranslationActivity {
    TranslationActivity() { g_shader_translations_running.fetch_add(1, std::memory_order_relaxed); }
    ~TranslationActivity() { g_shader_translations_running.fetch_sub(1, std::memory_order_relaxed); }
    TranslationActivity(const TranslationActivity&) = delete;
    TranslationActivity& operator=(const TranslationActivity&) = delete;
};
} // namespace

bool CompilationInProgress() {
    return PipelineBuildsRunning() != 0 ||
           g_shader_translations_running.load(std::memory_order_relaxed) != 0;
}

#ifdef RYTY_GPU
bool RytyGtsInRace(); // FIX-110, ryty_game_patch.cpp
bool RytyGtsMenuCaptureCanContinue();
bool RytyGtsMenuCaptureAvailable();
#endif

// PERF-310: in a race, shader translations and pipeline builds run below the game's priority.
// On CPUs without efficiency cores PERF-097 keeps nothing apart: a burst of new shaders on a
// 6-core Ryzen took 3-4 threads from GT Sport's 12 job threads and the recorder, and the race
// fell from 30 to 12-17 fps until it was done. Draws are skipped while their pipeline builds
// (PERF-076), so the compiles only take longer. RYGT_COMPILE_PRIORITY=normal keeps them level.
static bool CompileBelowGame() {
#ifdef RYTY_GPU
    static const bool enabled = [] {
        const char* env = std::getenv("RYGT_COMPILE_PRIORITY");
        return !(env && std::string_view{env} == "normal");
    }();
    return enabled && RytyGtsInRace();
#else
    return false;
#endif
}

static void ReportShaderAllocationFailure(Shader::HwStage stage, u64 hash, VAddr base,
                                           size_t words, const char* phase) {
    static thread_local u64 failures = 0;
    if (++failures > 3 && !std::has_single_bit(failures)) {
        return;
    }
    u64 available_commit = 0;
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        available_commit = memory.ullAvailPageFile;
    }
#endif
    LOG_ERROR(Render_Vulkan,
              "SHADER-ALLOCATION-FAIL: {} shader {:#x} at {:#x}, {} dwords, phase {}, "
              "decoded instruction size {} bytes, available commit {} MB; draw can retry later",
              stage, hash, base, words, phase, sizeof(Shader::Gcn::GcnInst), available_commit >> 20);
}

// The key for a new module must describe the bytes translation actually used.
// Looking up an existing module still reads the current draw's constants normally.
static void KeepCompiledTessellationLayout(Shader::StageSpecialization& spec,
                                           const Shader::RuntimeInfo& compiled) {
    if (compiled.sw_stage == Shader::SwStage::TessellationEval) {
        spec.runtime_info.sw.tes.hs_output_cp_stride = compiled.sw.tes.hs_output_cp_stride;
    } else if (compiled.sw_stage == Shader::SwStage::TessellationControl) {
        spec.runtime_info.sw.tcs.ls_stride = compiled.sw.tcs.ls_stride;
        spec.runtime_info.sw.tcs.hs_output_cp_stride = compiled.sw.tcs.hs_output_cp_stride;
        spec.runtime_info.sw.tcs.hs_output_base = compiled.sw.tcs.hs_output_base;
    }
}

extern "C" __declspec(dllexport) int RytyTestTessLayoutKeys() {
    unsigned cases=0;
    for (u32 stride : {0u, 24u, 128u, 160u, 65536u}) {
        Shader::StageSpecialization key{};
        Shader::RuntimeInfo compiled{};
        compiled.Initialize(Shader::HwStage::Vertex, Shader::SwStage::TessellationEval);
        compiled.sw.tes.hs_output_cp_stride=stride;
        key.runtime_info=compiled;
        key.runtime_info.sw.tes.hs_output_cp_stride=0x3f800000;
        KeepCompiledTessellationLayout(key,compiled);
        if(key.runtime_info.sw.tes.hs_output_cp_stride!=stride)return -1;
        ++cases;
        compiled.Initialize(Shader::HwStage::Hull, Shader::SwStage::TessellationControl);
        compiled.sw.tcs.hs_output_cp_stride=stride;
        compiled.sw.tcs.ls_stride=24;
        compiled.sw.tcs.hs_output_base=0x700;
        key.runtime_info=compiled;
        key.runtime_info.sw.tcs.hs_output_cp_stride=0xbf800000;
        key.runtime_info.sw.tcs.ls_stride=0x3f800000;
        key.runtime_info.sw.tcs.hs_output_base=0x3f800000;
        key.runtime_info.sw.tcs.num_threads=4;
        KeepCompiledTessellationLayout(key,compiled);
        if(key.runtime_info.sw.tcs.hs_output_cp_stride!=stride || key.runtime_info.sw.tcs.ls_stride!=24 || key.runtime_info.sw.tcs.hs_output_base!=0x700 || key.runtime_info.sw.tcs.num_threads!=4)return -2;
        ++cases;
    }
    Shader::StageSpecialization key{};Shader::RuntimeInfo compiled{};
    compiled.Initialize(Shader::HwStage::Vertex,Shader::SwStage::Vertex);
    key.runtime_info=compiled;key.runtime_info.sw.vs.step_rate_0=19;
    KeepCompiledTessellationLayout(key,compiled);
    if(key.runtime_info.sw.vs.step_rate_0!=19)return -3;
    return cases+1;
}

// Frontend resolution runs under the caller's captured draw state. Workers must
// never follow guest descriptor-table pointers after the handoff.
struct OwnedTranslation {
    Shader::Pools pools;
    Shader::RuntimeInfo compiled_runtime;
    std::optional<Shader::IR::Program> program;
    explicit OwnedTranslation(const Shader::RuntimeInfo& runtime) : compiled_runtime{runtime} {}
};

extern "C" __declspec(dllexport) int RytyTestOwnedTranslationInputs() {
    unsigned cases{};
    for (auto stage : {Shader::SwStage::TessellationControl, Shader::SwStage::TessellationEval}) {
        for (u32 stride : {24u, 128u, 160u, 65536u}) {
            Shader::Info info{};
            info.sw_stage = stage;
            Shader::RuntimeInfo runtime{};
            runtime.Initialize(stage == Shader::SwStage::TessellationControl ? Shader::HwStage::Hull
                                                                           : Shader::HwStage::Vertex,
                               stage);
            auto owned = std::make_shared<OwnedTranslation>(runtime);
            if (stage == Shader::SwStage::TessellationControl) {
                owned->compiled_runtime.sw.tcs.ls_stride = 24;
                owned->compiled_runtime.sw.tcs.hs_output_cp_stride = stride;
                owned->compiled_runtime.sw.tcs.hs_output_base = 0x700;
            } else {
                owned->compiled_runtime.sw.tes.hs_output_cp_stride = stride;
            }
            // A default Info has no usable tessellation V# or user-data table.
            // Construction must consume the owned layout without accessing either.
            Shader::Profile profile{};
            bool passed{};
            std::thread worker([owned, &info, &profile, runtime, stage, stride, &passed] {
                Shader::StageSpecialization key(info, runtime, profile, {}, nullptr,
                                                &owned->compiled_runtime);
                passed = stage == Shader::SwStage::TessellationControl
                    ? key.runtime_info.sw.tcs.hs_output_cp_stride == stride &&
                      key.runtime_info.sw.tcs.ls_stride == 24 && key.runtime_info.sw.tcs.hs_output_base == 0x700
                    : key.runtime_info.sw.tes.hs_output_cp_stride == stride;
            });
            worker.join();
            if (!passed) return -1;
            ++cases;
        }
    }
    return cases;
}

class PipelineCache::TranslateWorkers {
public:
    TranslateWorkers(const Shader::Profile& profile_, vk::Device device_, u32 count, bool keep_code_)
        : profile{profile_}, device{device_}, keep_code{keep_code_} {
        for (u32 i = 0; i < count; ++i) {
            threads.emplace_back([this, i](std::stop_token stop) { Run(stop, i); });
        }
    }
    ~TranslateWorkers() {
        for (auto& thread : threads) {
            thread.request_stop();
        }
        cv.notify_all();
    }

    void Prioritize(const std::shared_ptr<TranslateJob>& job) {
        {
            std::scoped_lock lk{mutex};
            RequireTranslation(queue, job);
        }
        cv.notify_one();
    }

    void Queue(std::shared_ptr<TranslateJob> job) {
        {
            std::scoped_lock lk{mutex};
            job->queued_at = std::chrono::steady_clock::now();
            EnqueueTranslation(queue, std::move(job), MaxAge().count() > 0);
        }
        cv.notify_one();
    }

private:
    static std::chrono::milliseconds MaxAge() {
        static const auto max_age = [] {
            return ParseTranslationMaxAge(std::getenv("RYGT_TRANSLATE_MAX_AGE_MS"));
        }();
        return max_age;
    }

    void Run(std::stop_token stop, u32 index) {
        Common::SetCurrentThreadName(fmt::format("shadGT:ShaderTranslate{}", index).c_str());
        if (Common::HybridCpu()) Common::PreferCompileCores();
        SCOPE_EXIT { Common::ReleaseCompileCores(); };
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Normal);
        bool low_priority = false;
        while (!stop.stop_requested()) {
            std::shared_ptr<TranslateJob> job;
            bool expired = false;
            {
                std::unique_lock lk{mutex};
                cv.wait(lk, stop, [this] { return !queue.empty(); });
                if (queue.empty()) {
                    continue;
                }
                std::tie(job, expired) = TakeTranslation(queue, std::chrono::steady_clock::now(), MaxAge());
            }
            if (expired) continue;
            const bool want_low = Common::CompileThreadsLow();
            if (want_low != low_priority) {
                low_priority = want_low;
                Common::SetCurrentThreadPriority(want_low ? Common::ThreadPriority::Low
                                                          : Common::ThreadPriority::Normal);
            }
            try {
                // Release temporary IR allocations after each job, including failed jobs.
                Shader::Pools pools;
                Common::RecoverableScope recoverable;
                Translate(*job, pools);
            } catch (const std::bad_alloc&) {
                ReportShaderAllocationFailure(job->hw_stage, job->hash, job->code_base,
                                              job->code.size(), "background translation");
            } catch (const Common::RecoverableFailure& error) {
                LOG_WARNING(Render_Vulkan, "SHADER-WORKER-FAIL: shader {:#x}: {}", job->hash,
                            error.what());
            }
            {
                std::scoped_lock lk{job->mutex};
                if (!job->module) {
                    job->retry_after = std::chrono::steady_clock::now() + std::chrono::milliseconds{250};
                }
                job->done = true;
            }
            job->cv.notify_all();
        }
    }

    void Translate(TranslateJob& job, Shader::Pools& pools) {
        TranslationActivity activity;
        auto prepared = std::static_pointer_cast<OwnedTranslation>(job.prepared_translation);
        ASSERT(prepared && prepared->program && job.info);
        auto& info = job.info;
        auto binding = job.start;
#ifndef NDEBUG
        const auto start = std::chrono::steady_clock::now();
#endif
        try {
            Common::RecoverableScope recoverable;
            job.spv = Shader::Backend::SPIRV::EmitSPIRV(
                profile, prepared->compiled_runtime, *prepared->program, binding);
            job.spec = Shader::StageSpecialization(*info, job.runtime_info, profile, job.start,
                                                   info->translated_fetch_shader.get(), &prepared->compiled_runtime);
            KeepCompiledTessellationLayout(job.spec, prepared->compiled_runtime);
        } catch (const Common::RecoverableFailure&) {
            job.prepared_translation.reset();
            return;
        }
        job.prepared_translation.reset();
        if (!Common::PerfFeatureEnabled(44)) {
            info->resource_usage_known = false;
        }
        job.module = CompileSPV(job.spv, device, keep_code);
#ifndef NDEBUG
        job.translate_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
#endif
    }

    const Shader::Profile& profile;
    vk::Device device;
    const bool keep_code;
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<std::shared_ptr<TranslateJob>> queue;
    std::vector<std::jthread> threads;
};


static bool SaneTessConstants(const Shader::Info& hull, const AmdGpu::Liverpool* liverpool,
                              bool read_ahead); // FIX-058
#ifdef RYTY_GPU
void RytyWatchWrites(std::uint64_t address, std::uint32_t ms, std::string label); // DIAG-067
#endif

// PERF-098: precompile progress.
static std::atomic<u64> g_precompile_total{};
static std::atomic<u64> g_precompile_done{};
static std::atomic<bool> g_precompile_reading{};

// PERF-102: while the precompile runs at start, the game waits on the splash screen (its clocks
// held, so it sees no time pass), as if the splash screen were frozen until it is done.
static std::atomic<bool> g_precompile_hold{};

bool PrecompileHoldsGame() {
    return g_precompile_hold.load(std::memory_order_relaxed);
}

static void ReleasePrecompileHold() {
    if (g_precompile_hold.exchange(false)) {
        Common::GuestClock::ReleaseHold();
        LOG_WARNING(Render_Vulkan, "PERF-102: precompile done; the game continues");
    }
}

// PERF-103: compile slow motion. While draws are skipped because their pipeline is still being
// built, the guest clocks run at a quarter speed: held 3 ms of every 4 (no vblank passes while
// held, so the game's frame rate drops with it), so a long compile looks like slow motion.
static std::atomic<s64> g_last_skip_ns{};

static u32 CompileMode() {
#ifdef RYTY_GPU
    static const u32 mode = static_cast<u32>(std::clamp(EmulatorSettings.GetCompileMode(), 0, 2));
    return mode;
#else
    return 2;
#endif
}

// Opt-in for animated menu scenes such as the gift-car roulette. Normal menus retain
// their wait policy because some render into a texture only once.
static bool AsyncMenuPipelines() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADGT_ASYNC_MENUS");
        const bool on = env && env[0] == '1';
        if (on) {
            LOG_INFO(Render_Vulkan,
                     "GIFT-RECOVERY: animated menus build graphics pipelines asynchronously");
        }
        return on;
    }();
    return enabled;
}

static void NoteSkippedDraw() {
    g_last_skip_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                         std::memory_order_relaxed);
}

#ifdef RYTY_GPU
bool RytyGtsInRace(); // FIX-110, ryty_game_patch.cpp
#endif

bool CompileSlowMotion() {
    const s64 last = g_last_skip_ns.load(std::memory_order_relaxed);
#ifdef RYTY_GPU
    // FIX-113: a race keeps going at full speed while its new pipelines build (objects show a
    // moment later instead of the race slowing down). -DisablePerf 136 slows races down too.
    static const bool race_keeps_going = Common::PerfFeatureEnabled(136);
    if (race_keeps_going && RytyGtsInRace()) {
        return false;
    }
#endif
    return CompileMode() == 1 && last != 0 &&
           std::chrono::steady_clock::now().time_since_epoch().count() - last < 50'000'000;
}

static void SlowMotionThread(std::stop_token stop) {
    Common::SetCurrentThreadName("shadGT:CompileSlowMotion");
    using namespace std::chrono_literals;
    while (!stop.stop_requested()) {
        if (!CompileSlowMotion()) {
            std::this_thread::sleep_for(5ms);
            continue;
        }
        Common::GuestClock::TakeHold();
        std::this_thread::sleep_for(3ms);
        Common::GuestClock::ReleaseHold();
        std::this_thread::sleep_for(1ms);
    }
}

PrecompileProgress GetPrecompileProgress() {
    const u64 total = g_precompile_total.load(std::memory_order_relaxed);
    const u64 done = std::min(total, g_precompile_done.load(std::memory_order_relaxed));
    return {total, done, total != 0 && done < total};
}

// PERF-019: worker threads that build graphics pipelines ahead of the draws that need them.
// PERF-038: stored pipelines are built in the background on a few of them, at low priority.
// PERF-GT7-019: read metadata and prefetch bounded SPIR-V files off the recorder.
class PipelineCache::StoreReaders {
public:
    explicit StoreReaders(u32 count)
        : readers(count, [](u64 hash) { return ReadStoredListing(hash); },
            [](u64 hash, size_t index) {
                std::vector<u32> words;
                Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                    fmt::format("{:#018x}_{}", hash, index), words);
            }, [](u32 index) {
                Common::SetCurrentThreadName(fmt::format("shadGT:ShaderStore{}", index).c_str());
            }) {}
    std::future<StoredListing> List(u64 hash) { return readers.List(hash); }
private:
    ShaderStoreReaders<StoredListing> readers;
};

class PipelineCache::PipelineBuildWorkers {
public:
    PipelineBuildWorkers()
        : queue{NumWorkers(), std::max(1u, NumWorkers() / 4), StartupWorkers()} {
        // Translation and pipeline pools share a topology-derived CPU budget.
        // Idle workers sleep; compiler priority leaves critical game threads first choice.
        for (u32 i = 0; i < NumWorkers(); ++i) {
            threads.emplace_back([this](std::stop_token stop) { Work(stop); });
        }
    }
    ~PipelineBuildWorkers() {
        if (catch_up) SetCatchUpPolicy(false);
        for (auto& thread : threads) {
            thread.request_stop();
        }
        queue.NotifyAll();
    }

    void Push(std::function<void()>&& job, BuildQueue::Kind kind, bool heavy = false) {
        queue.Push(std::move(job), kind, heavy);
    }
    void PushBackground(std::function<void()>&& job, bool heavy = false) {
        queue.Push(std::move(job), BuildQueue::Kind::Background, heavy);
    }
    void PauseBackground(BuildQueue::Clock::time_point until) {
        queue.PauseBackground(until);
    }
    void SetBoost(bool on) {
        if (on && threads.size() < StartupWorkers()) {
            // Only startup creates extra workers. They sleep behind the shared
            // gameplay admission limit after the splash screen ends.
            const auto previous = threads.size();
            while (threads.size() < StartupWorkers()) {
                threads.emplace_back([this](std::stop_token stop) { Work(stop); });
            }
            LOG_INFO(Render_Vulkan, "PRECOMPILE-CPU: {} startup workers (was {}); "
                     "{} gameplay pipeline workers; heavy limit {}",
                     threads.size(), previous, NumWorkers(), CompilerWorkerBudget::HeavyPipelineLimit);
        }
        queue.SetBoost(on);
    }
    static u32 UsualWidth() { return NumWorkers(); }
    void SetPace(u32 limit) { queue.SetReadAheadLimit(limit); }
    bool SetCatchUp(bool on) {
        if (on == catch_up) return true;
        if (on) {
            const u32 width = std::max(NumWorkers(), std::min(8u, StartupWorkers()));
            if (!TryGrowCompileWorkers(threads, width, [this] {
                    threads.emplace_back([this](std::stop_token stop) { Work(stop); });
                })) {
                LOG_WARNING(Render_Vulkan, "Catch-up workers unavailable; retaining normal admission");
                return false;
            }
            SetCatchUpPolicy(true);
            queue.SetForegroundLimit(width);
        } else {
            queue.SetForegroundLimit(NumWorkers());
            SetCatchUpPolicy(false);
        }
        catch_up = on;
        return true;
    }
    bool Busy() { return queue.Busy(); }

    size_t BackgroundQueued() {
        return queue.BackgroundQueued();
    }
    bool ForegroundIdle() {
        return queue.ForegroundIdle();
    }

private:
    static void SetCatchUpPolicy(bool on) {
        static const bool low = [] {
            const char* value = std::getenv("RYGT_CATCH_UP_LOW");
            return !(value && value[0] == '0');
        }();
        if (low) Common::SetCompileCatchUp(on);
        else Common::SetCompileCoresWide(on);
    }

    static u32 NumWorkers() {
        return CompilerWorkerBudget::ForLogicalThreads(
            std::thread::hardware_concurrency()).pipeline;
    }

    static u32 StartupWorkers() {
        return CompilerWorkerBudget::StartupPipelineWorkers(std::thread::hardware_concurrency());
    }

    void Work(std::stop_token stop) {
        Common::SetCurrentThreadName("shadGT:PipelineBuild");
        Common::PreferCompileCores();
        SCOPE_EXIT { Common::ReleaseCompileCores(); };
        bool all_cores = false;
        bool low_priority = false;
        bool backlog = false;
        static const bool backlog_cores = Common::PerfFeatureEnabled(160);
        while (true) {
            BuildQueue::Job job;
            bool background = false, heavy = false, read_ahead = false;
            if (!queue.Pop(job, background, stop, &heavy, &read_ahead)) {
                return;
            }
            if (backlog_cores) {
                const size_t pending = queue.Pending();
                if (pending >= 16) backlog = true;
                else if (pending <= 2) backlog = false;
            }
            const bool want_all = (backlog && Common::HybridCpu()) || Common::CompileCoresWide();
            if (want_all != all_cores) {
                all_cores = want_all;
                if (all_cores) Common::ReleaseCompileCores();
                else Common::PreferCompileCores();
            }
            const bool want_low = backlog || Common::CompileThreadsLow();
            if (want_low != low_priority) {
                low_priority = want_low;
                Common::SetCurrentThreadPriority(want_low ? Common::ThreadPriority::Low
                                                          : Common::ThreadPriority::Normal);
            }
            job();
            if (background) {
                queue.FinishedBackground(heavy);
            } else {
                queue.FinishedForeground(heavy, read_ahead);
            }
        }
    }

    BuildQueue queue;
    std::vector<std::jthread> threads;
    bool catch_up{};
};

// PERF-019: what a worker needs to build one graphics pipeline. The shader infos are copied
// (with their user data) because the command thread keeps changing the cached ones.
struct PipelineCache::PipelineBuild {
    GraphicsPipelineKey key{};
    std::array<Shader::Info, MaxShaderStages> info_copies{};
    std::array<std::vector<u32>, MaxShaderStages> user_data{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::optional<Shader::Gcn::FetchShaderData> fetch;
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    GraphicsPipeline::SerializationSupport sdata{};
    std::unique_ptr<GraphicsPipeline> pipeline;
    std::promise<void> promise;
    std::shared_future<void> done;
    std::chrono::steady_clock::time_point queued;
    std::atomic<bool> urgent{};
    // PERF-038: a stored pipeline built in the background (no runtime shader state needed).
    bool preloading{};
    // PERF-076: a new pipeline its draws skip until a worker has built it.
    bool async{};
    u32 required_waiters{}; // lookup mutex only; never touched by compiler workers
    // A worker claims the build once; cancellation claims an unstarted job without compiling.
    std::atomic<bool> started{};
    std::atomic<bool> bumped{};
    std::atomic<u64> skips{};
    std::atomic<s64> last_skip_ms{};
    // The cached infos the copies were made from; the pipeline points at them once built.
    std::array<const Shader::Info*, MaxShaderStages> canonical{};
};

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

/// SHD-004: a color target as far as a pixel shader's translation reads it: the export format,
/// the number class of the output, the conversion, which source channel each channel takes (a
/// constant channel, as formats without alpha have, takes its own, as the translation does) and
/// the blend emulation. A target the draw doesn't bind is translated as a float target with the
/// usual packed export (its writes go nowhere: the pass has no attachment there), so a pixel
/// shader translates the same into passes with one target or two and into float targets of other
/// formats (two in five of GT Sport's pixel shader translations differed only in that).
static Shader::PsColorBuffer TranslationColorBuffer(const Shader::PsColorBuffer& cb,
                                                    bool unorm_fixup) {
    if (unorm_fixup || cb.blend_self_scale || cb.blend_swizzled_alpha ||
        cb.blend_swizzled_factors) {
        return cb; // these read the target's format too
    }
    Shader::PsColorBuffer out{};
    out.data_format = AmdGpu::DataFormat::Format16_16_16_16;
    if (cb.data_format == AmdGpu::DataFormat::FormatInvalid) {
        out.num_format = AmdGpu::NumberFormat::Float;
        out.export_format = AmdGpu::ShaderExportFormat::ABGR_FP16;
        out.swizzle = AmdGpu::IdentityMapping;
        return out;
    }
    switch (AmdGpu::GetNumberClass(cb.num_format)) {
    case AmdGpu::NumberClass::Uint:
        out.num_format = AmdGpu::NumberFormat::Uint;
        break;
    case AmdGpu::NumberClass::Sint:
        out.num_format = AmdGpu::NumberFormat::Sint;
        break;
    default:
        out.num_format = AmdGpu::NumberFormat::Float;
        break;
    }
    out.num_conversion = cb.num_conversion;
    out.export_format = cb.export_format;
    for (u32 i = 0; i < 4; ++i) {
        out.swizzle.array[i] =
            AmdGpu::CompSwizzle(u32(AmdGpu::CompSwizzle::Red) + cb.swizzle.Map(i));
    }
    return out;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(HwStage stage, SwStage l_stage) {
    auto& info = S().runtime_infos[u32(l_stage)];
    const auto& regs = Regs();
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = S().infos[u32(SwStage::TessellationControl)];
        if (hull_info) { // FIX-058: no hull info when its draw is skipped
            hull_info->ReadTessConstantBuffer(tess_constants);
        }
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        static const bool direct_barycentric_requested = [] {
            const char* value = std::getenv("RYGT_AMD_SINGLE_SAMPLE_BARYCENTRIC");
            return !value || std::strcmp(value, "0") != 0;
        }();
        info.hw.fs.single_sample_barycentric = UseSingleSampleBarycentric(
            instance.GetVendorID(), profile.supports_fragment_shader_barycentric,
            S().graphics_key.num_samples, direct_barycentric_requested);
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.quad_sample_mask =
            regs.reg_array[0xa30e] != ~0u || regs.reg_array[0xa30f] != ~0u;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        info.hw.fs.dual_source_blending |= S().graphics_key.color_buffers[0].blend_swizzled_alpha ||
                                           S().graphics_key.color_buffers[0].blend_swizzled_factors;
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = TranslationColorBuffer(
                S().graphics_key.color_buffers[i], profile.needs_unorm_fixup); // SHD-004
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        // AMD-307: the vertex order check takes locations 30/31. The stage feeding the
        // rasterizer exports NumExports() parameters, at most two locations further on (clip
        // distances, layer), so the pixel shader reads the check only when those stay below 30;
        // the vertex side checks its own outputs the same way and then always writes it.
        info.hw.fs.vertex_order_check =
            profile.vertex_order_check && !IsTessEmulatedDraw() &&
            regs.vs_output_config.NumExports() + 2 <= 30; // OrderCheckRefLocation
        break;
    }
    case HwStage::Compute: {
        const auto& cs_pgm = liverpool->DrawCsRegs();
        info.props.num_user_data = cs_pgm.settings.num_user_regs;
        info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                     cs_pgm.num_thread_z.full};
        info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                  cs_pgm.IsTgidEnabled(2)};
        info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = S().draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = S().draw_indirect_params.instance_sgpr_offset;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            (regs.primitive_type == AmdGpu::PrimitiveType::QuadList && !QuadListAsTriangles());
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        info.sw.tcs.lds_emulation = u8(TessLdsEmulation(regs));
        info.sw.tcs.num_patches = u8(info.sw.tcs.lds_emulation ? regs.ls_hs_config.num_patches : 0);
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

static bool WarmComputeOn() {
    static const bool on = [] { const char* p = std::getenv("RYGT_WARM_COMPUTE");
                                return !(p && p[0] == '0'); }();
    return on;
}

std::filesystem::path PipelineCache::WarmComputeDir() {
    const auto& serial = Common::Singleton<Common::ElfInfo>::Instance()->GameSerial();
    return Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
           fmt::format("{}.warm_compute", serial);
}

void PipelineCache::WarmComputePipelines(std::stop_token stop) {
    Common::SetCurrentThreadName("shadGT:WarmCompute");
    Common::PreferCompileCores();
    SCOPE_EXIT { Common::ReleaseCompileCores(); };
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    const auto policy_suffix = fmt::format("-{:#018x}", ComputePipeline::RecipePolicy(instance, profile));
    const auto files = ListComputeWarmRecipes(WarmComputeDir(), policy_suffix);
    u32 warmed = 0;
    for (const auto& file : files) {
        if (stop.stop_requested()) return;
        if (ComputePipeline::WarmFromRecipe(instance, profile, *pipeline_cache, file, stop)) ++warmed;
    }
    LOG_INFO(Render_Vulkan, "PERF-GT7-037: warmed {} of {} compute recipes", warmed, files.size());
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
    if (liverpool) {
        liverpool->on_command_buffer_start = [this] { ReadAheadAtBufferStart(); };
    }
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        // FIX-223 (RYGT_DISCARD_KILL=0 or 1 overrides): AMD ends discarded pixels at once.
        .discard_kills = [&] {
            const char* env = std::getenv("RYGT_DISCARD_KILL");
            return env && *env ? *env == '1'
                               : instance.GetDriverID() == vk::DriverId::eAmdProprietary;
        }(),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .amd_vertex_order = instance_.GetAmdVertexOrder(), // AMD-311
        // AMD-313: FIX-215/216 drew the purple car again on an RX 7600 (fresh store), so RX 7000
        // cards leave them out; SHADGT_AMD_NAN_RULES=0/1 overrides for any GPU.
        .amd_nan_rules =
            [] {
                if (const char* env = std::getenv("SHADGT_AMD_NAN_RULES"); env && *env) {
                    return env[0] != '0';
                }
                return GetStoreGpuFamily() != StoreGpuFamily::AmdRdna3;
            }(),
        .supports_quad_derivatives = instance_.IsQuadDerivativesSupported(), // AMD-306
        .vertex_order_check = instance_.IsVertexOrderCheckEnabled(),         // AMD-307
        // AMD-315: on AMD's explicit vertex path a V_INTERP_MOV makes the whole attribute
        // per-vertex, and the other components' V_INTERP_P2 reads then return vertex 0: flat,
        // single-colour triangles (hills at Fuji on an RX 6600 XT). AMD's older compiler keeps
        // that path (AMD-314), so it does GCN's P1/P2 arithmetic by hand, as 217d4011 does for
        // the KHR path. SHADGT_MANUAL_INTERP=0/1 overrides.
        .needs_manual_interpolation =
            [&] {
                if (const char* env = std::getenv("SHADGT_MANUAL_INTERP"); env && *env) {
                    return env[0] != '0';
                }
                return (instance.IsFragmentShaderBarycentricSupported() &&
                        instance.GetDriverID() == vk::DriverId::eNvidiaProprietary) ||
                       (instance.IsAmdShaderExplicitVertexParameterSupported() &&
                        instance.IsAmdLegacyShaderCompiler());
            }(),
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .intel_arc_workarounds = instance.IsIntel(),
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
    };
    if (const char* value = std::getenv("RYGT_AMD_SINGLE_SAMPLE_BARYCENTRIC");
        (!value || std::strcmp(value, "0") != 0) && instance.GetVendorID() == 0x1002) {
        LOG_WARNING(Render_Vulkan, "AMD-BARY: single-sample compiler path enabled; KHR {}",
                    profile.supports_fragment_shader_barycentric);
    }
    // D2: read-only guest buffers are read in place through the direct-memory page table.
    profile.direct_buffers =
        VideoCore::DirectMemory::Enabled() && instance.IsExternalMemoryHostSupported();
#ifdef RYTY_GPU
    profile.native_cube_sampling = NativeModeRequested() && instance.IsImageCubeArraySupported();
    SetNativeCubeCacheEnabled(profile.native_cube_sampling);
#endif
    const char* barycentric_env = std::getenv("RYGT_AMD_SINGLE_SAMPLE_BARYCENTRIC");
    SetVertexOrderCheckCacheEnabled(profile.vertex_order_check); // AMD-307
    SetDiscardKillCacheEnabled(profile.discard_kills);           // FIX-223
    SetSingleSampleBarycentricCacheEnabled(UseSingleSampleBarycentric(
        instance.GetVendorID(), profile.supports_fragment_shader_barycentric, 1,
        !barycentric_env || std::strcmp(barycentric_env, "0") != 0));
    CreateDriverCache();
    if (WarmComputeOn()) {
        try {
            warm_compute_thread = std::jthread([this](std::stop_token stop) {
                try { WarmComputePipelines(stop); }
                catch (const std::exception& e) { LOG_WARNING(Render_Vulkan, "Warm compute stopped: {}", e.what()); }
                catch (...) { LOG_WARNING(Render_Vulkan, "Warm compute stopped unexpectedly"); }
            });
        } catch (const std::exception& e) {
            LOG_WARNING(Render_Vulkan, "Warm compute unavailable: {}", e.what());
        }
    }
    // PERF-032: the shader store is used again, without the startup precompile Lance had removed:
    // a program's stored translations are read when a draw first uses it, and new translations
    // are stored. -DisablePerf 47 leaves the store closed.
    // FIX-186 (RyGT): at any RAM setting. The launcher's 16 GB setting turns
    // pipeline_cache_enabled off for the driver's cache file, and that closed the store too, so
    // with 16 GB nothing was written to or read from the shaders folder (Lance, 2026-10-21).
#ifdef RYTY_GPU
    const char* shader_store = std::getenv("RYGT_SHADER_STORE");
    const bool store_wanted = !(shader_store && shader_store[0] == '0');
#else
    const bool store_wanted = EmulatorSettings.IsPipelineCacheEnabled();
#endif
#ifndef NDEBUG
    // Instrumented shader binaries share ordinary shader keys. Keep this Debug run
    // out of the persistent store so later normal runs cannot load them.
    const char* nan_trace = std::getenv("RYGT_NAN_TRACE");
    const bool instrumenting = nan_trace && std::strtoull(nan_trace, nullptr, 16) != 0;
#else
    constexpr bool instrumenting = false;
#endif
    if (store_wanted && !instrumenting && Common::PerfFeatureEnabled(47)) {
        if (EmulatorSettings.IsPipelineCacheArchived()) {
            LOG_WARNING(Render_Vulkan, "PERF-032: the shader store is not used with an archived "
                                       "pipeline cache (pipeline_cache_archived)");
        } else {
            Storage::DataBase::Instance().Open();
            use_stored_shaders = Storage::DataBase::Instance().IsOpened();
            if (use_stored_shaders) LoadElseScopeFixed();
            const char* readers = std::getenv("RYGT_STORE_READERS");
            if (use_stored_shaders && !(readers && readers[0] == '0')) {
                try { store_readers = std::make_unique<StoreReaders>(8); }
                catch (const std::exception& e) {
                    LOG_WARNING(Render_Vulkan, "Shader store readers unavailable, using synchronous reads: {}", e.what());
                }
            }
            // D10: the old renderer's translations stand in while the new renderer's are built.
            // SHADGT_LEGACY_FALLBACK=0 turns it off.
            // SHD-002: not with stage descriptor sets: the old translations bind everything
            // in set 0.
            if (profile.direct_buffers && false) {
                const char* fallback_env = std::getenv("SHADGT_LEGACY_FALLBACK");
                if (!fallback_env || fallback_env[0] != '0') {
                    Storage::DataBase::Legacy().OpenLegacy();
                    legacy_fallback = Storage::DataBase::Legacy().IsOpened();
#ifndef NDEBUG
                    legacy_stats.since = std::chrono::steady_clock::now();
#endif
                    LOG_WARNING(Render_Vulkan, "D10: old-renderer shader fallback {}",
                                legacy_fallback ? "on" : "off (no old shader store)");
                }
            }
            LOG_WARNING(Render_Vulkan,
                        "PERF-032: shader translations are loaded from the store on first use");
            // PERF-038: opt-in (SHADGT_PREWARM=1): the stored pipelines are built in the
            // background while the game runs, most recently used first. -DisablePerf 52. ryGT
            // turns it on unless SHADGT_PREWARM=0, so GTSport.exe runs without a launcher.
            const char* prewarm = std::getenv("SHADGT_PREWARM");
#ifdef RYTY_GPU
            // PERF-098: GPU.precompile_shaders (on by default) unless the variable says otherwise.
            const bool prewarm_on =
                prewarm ? prewarm[0] != '0' : EmulatorSettings.IsPrecompileShaders();
#else
            const bool prewarm_on = prewarm && prewarm[0] == '1';
#endif
            if (prewarm_on && Common::PerfFeatureEnabled(52)) {
                if (!instance.IsVertexInputDynamicState()) {
                    LOG_WARNING(Render_Vulkan, "PERF-038: background pipeline builds need dynamic "
                                               "vertex input; not available here");
                } else {
                    prewarm_enabled = true;
#ifndef NDEBUG
                    prewarm_stats.since = std::chrono::steady_clock::now();
#endif
                    prewarm_reader =
                        std::jthread{[this](std::stop_token stop) { PrewarmRead(stop); }};
                }
            }
        }
    }
    if (CompileMode() == 1 && AsyncPipelines()) {
        static std::jthread slow_motion{SlowMotionThread}; // PERF-103
    }
#ifndef NDEBUG
    // PB-1 (SHADGT_REPLAY_TEST=<folder of <hash>.bin programs>): translates every stored shader
    // again from its program and its stored record, and counts how many match the stored SPIR-V.
    if (const char* codes = std::getenv("SHADGT_REPLAY_TEST"); codes && *codes) {
        static std::jthread replay_test{[this, dir = std::string{codes}] { ReplayTest(dir); }};
    }
    if (const char* dir = std::getenv("SHADGT_TEMPLATE_TEST"); dir && *dir) {
        static std::jthread template_test{[this, d = std::string{dir}] { TemplateTest(d); }};
    }
    if (const char* dir = std::getenv("SHADGT_DUMP_STORE"); dir && *dir) {
        static std::jthread dump_store{[this, d = std::string{dir}] { DumpStore(d); }};
    }
#endif
    // PERF-GT7-028: up to half the logical CPUs translate shaders; the
    // independent heavy-driver-compile admission limit remains in force.
    if (const u32 count = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 8u);
        Common::PerfFeatureEnabled(102)) {
        translate_workers =
            std::make_unique<TranslateWorkers>(profile, instance.GetDevice(), count,
                                               instance.IsShaderObjectEnabled());
#ifndef NDEBUG
        translate_stats.since = std::chrono::steady_clock::now();
#endif
    }
    if (!ExitCacheRegistry().Register(this))
        LOG_WARNING(Render_Vulkan, "CACHE-EXIT: another pipeline cache owns exit-save registration");
}

PipelineCache::~PipelineCache() {
    ExitCacheRegistry().Unregister(this); // Wait for an in-flight close save before teardown.
    if (warm_compute_thread.joinable()) {
        warm_compute_thread.request_stop();
        warm_compute_thread.join();
    }
    store_readers.reset(); // Join reads before the shader store and pipeline cache teardown.
    if (liverpool) {
        liverpool->on_command_buffer_start = nullptr;
    }
    // Join producers and compiler workers while their cache, shader modules and descriptor
    // heap are still alive. Do not serialize the driver cache concurrently with teardown.
    prewarm_reader.request_stop();
    if (prewarm_reader.joinable()) prewarm_reader.join();
    translate_workers.reset();
    build_workers.reset();
    // Shutdown only: deliver all GPU-complete handoffs and drain before device teardown.
    scheduler.Finish();
    scheduler.PopPendingOperations();
    PipelineRetirementWorker().Drain();
    SaveDriverCache(true);
}

// Bounded persistence is automatic; the environment variable is a diagnostic opt-out.
static bool DriverCacheFileEnabled() {
#ifndef NDEBUG
    const char* nan_trace = std::getenv("RYGT_NAN_TRACE");
    if (nan_trace && std::strtoull(nan_trace, nullptr, 16) != 0) return false;
#endif
#ifdef RYTY_GPU
    const char* env = std::getenv("RYGT_DRIVER_CACHE");
    return !env || env[0] != '0';
#else
    return EmulatorSettings.IsPipelineCacheEnabled();
#endif
}

// Verify the closed temporary file before replacing the previous cache.
static bool VerifyDriverCacheFile(const std::filesystem::path& path,
                                  std::span<const u8> expected) {
    try {
        Common::RecoverableScope recoverable;
    const Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read};
    if (!file.IsOpen() || file.GetSize() != expected.size()) return false;
    std::array<u8, 64 * 1024> buffer{};
    for (size_t offset = 0; offset < expected.size();) {
        const size_t count = std::min(buffer.size(), expected.size() - offset);
        if (file.ReadSpan<u8>(std::span<u8>{buffer.data(), count}) != count ||
            !std::equal(buffer.begin(), buffer.begin() + count, expected.begin() + offset))
            return false;
        offset += count;
    }
    return true;

    } catch (...) {
        return false;
    }
}

static bool WriteVerifiedDriverCacheFile(const std::filesystem::path& path,
                                       std::span<const u8> data) {
    try {
        Common::RecoverableScope recoverable;
    {
        const Common::FS::IOFile file{path, Common::FS::FileAccessMode::Create};
        if (!file.IsOpen() || file.WriteSpan<u8>(data) != data.size() || !file.Commit())
            return false;
    }
    return VerifyDriverCacheFile(path, data);

    } catch (...) {
        return false;
    }
}


// A rejected file is retained before a healthy replacement may be saved.
// No overwrite: existing distinct evidence remains intact. Four slots bound duplicates.
static bool QuarantineRejectedDriverCache(const std::filesystem::path& source,
                                          std::span<const u8> data) {
    try {
        Common::RecoverableScope recoverable;
    const u64 hash = XXH3_64bits(data.data(), data.size());
    for (u32 slot = 0; slot < 4; ++slot) {
        auto dest = source;
        dest += fmt::format(".rejected-{:016x}-{}", hash, slot);
        std::error_code ec;
        const bool exists = std::filesystem::exists(dest, ec);
        if (ec) return false;
        if (exists) {
            if (!VerifyDriverCacheFile(dest, data)) continue;
        } else {
            if (!std::filesystem::copy_file(source, dest, std::filesystem::copy_options::none, ec))
                continue;
        }
        {
            const Common::FS::IOFile durable{dest, Common::FS::FileAccessMode::ReadWrite};
            if (!durable.IsOpen() || !durable.Commit()) return false;
        }
        if (!VerifyDriverCacheFile(dest, data)) return false;
        LOG_WARNING(Render_Vulkan, "Rejected driver cache preserved at {}", dest.string());
        return true;
    }
    return false;

    } catch (...) {
        return false;
    }
}

// Preserve any cache that could not be verified or safely quarantined this run.
static std::atomic<bool> rejected_driver_cache{false};

void PipelineCache::CreateDriverCache() {
    std::vector<u8> data;
    try {
    Common::RecoverableScope recoverable;
    if (DriverCacheFileEnabled()) {
        const auto& serial = Common::Singleton<Common::ElfInfo>::Instance()->GameSerial();
        const auto identity = instance.GetPhysicalDevice().getProperties();
        std::string uuid;
        for (auto byte : identity.pipelineCacheUUID) uuid += fmt::format("{:02x}", byte);
        driver_cache_path = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
            fmt::format("{}-{:04x}-{:04x}-{:08x}-{}.vkpipelinecache", serial,
                        identity.vendorID, identity.deviceID, identity.driverVersion, uuid);
        std::error_code exists_error;
        const bool existing_cache = std::filesystem::exists(driver_cache_path, exists_error);
        if (exists_error) rejected_driver_cache.store(true);
        const Common::FS::IOFile file{driver_cache_path, Common::FS::FileAccessMode::Read};
        if (existing_cache && (!file.IsOpen() || file.GetSize() > DriverCacheDiskBudget))
            rejected_driver_cache.store(true);
        if (file.IsOpen() && file.GetSize() <= DriverCacheDiskBudget) {
            try {
                data.resize(file.GetSize());
                if (file.Read(data) != data.size()) {
                    rejected_driver_cache.store(true);
                    data.clear();
                }
            } catch (const std::bad_alloc&) {
                rejected_driver_cache.store(true);
                data.clear();
                LOG_WARNING(Render_Vulkan, "CACHE-LOAD-DEFER: allocation failed; starting with an empty cache");
            }
        }
        // Header (Vulkan spec, VkPipelineCacheHeaderVersionOne): size, version, vendor,
        // device, UUID. Data from another GPU or driver starts an empty cache.
        const auto props = instance.GetPhysicalDevice().getProperties();
        struct Header {
            u32 size;
            u32 version;
            u32 vendor_id;
            u32 device_id;
            std::array<u8, VK_UUID_SIZE> uuid;
        } header{};
        if (data.size() < sizeof(Header)) {
            if (existing_cache) rejected_driver_cache.store(true);
            data.clear();
        } else {
            std::memcpy(&header, data.data(), sizeof(Header));
            if (header.size < sizeof(Header) || header.size > data.size() ||
                header.version != static_cast<u32>(vk::PipelineCacheHeaderVersion::eOne) ||
                header.vendor_id != props.vendorID || header.device_id != props.deviceID ||
                std::memcmp(header.uuid.data(), props.pipelineCacheUUID.data(), VK_UUID_SIZE)) {
                LOG_INFO(Render_Vulkan, "Driver pipeline cache is from another GPU or driver");
                rejected_driver_cache.store(true);
                data.clear();
            }
        }
    }
    } catch (...) {
        rejected_driver_cache.store(true);
        data.clear();
        LOG_WARNING(Render_Vulkan, "CACHE-LOAD-DEFER: cache read failed; keeping original");
    }
    auto [cache_result, cache] =
        instance.GetDevice().createPipelineCacheUnique(vk::PipelineCacheCreateInfo{
            .initialDataSize = data.size(),
            .pInitialData = data.data(),
        });
    if (cache_result != vk::Result::eSuccess && !data.empty()) {
        LOG_WARNING(Render_Vulkan, "Driver pipeline cache rejected ({}), starting empty",
                    vk::to_string(cache_result));
        bool preserved = false;
        try {
            preserved = QuarantineRejectedDriverCache(driver_cache_path, data);
        } catch (...) {
            // Preservation is optional; retain the existing file on any failure.
        }
        if (!preserved) {
            rejected_driver_cache.store(true);
            LOG_WARNING(Render_Vulkan, "CACHE-SAVE-DEFER: could not preserve rejected cache; keeping original");
        }
        data.clear(); // The accepted fallback is empty; report the actual loaded size.
        auto [empty_result, empty_cache] = instance.GetDevice().createPipelineCacheUnique({});
        cache_result = empty_result;
        cache = std::move(empty_cache);
    }
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
    pipelines_at_driver_save = num_new_pipelines;
    driver_cache_saved_at = std::chrono::steady_clock::now();
    LOG_INFO(Render_Vulkan, "Driver pipeline cache: loaded {} bytes, hash {:016x}, file {}",
             data.size(), data.empty() ? 0ull : XXH3_64bits(data.data(), data.size()),
             driver_cache_path.string());
}

// A disk-cache snapshot is optional. Query its size before allocating, and never let an
// allocation failure in this optimization terminate the game. Unlike the enhanced Vulkan-Hpp
// overload, this makes a single bounded read rather than reallocating while the cache grows.
static std::vector<u8> ReadDriverCacheSnapshot(vk::Device device, vk::PipelineCache cache) {
    try {
        u64 available = std::numeric_limits<u64>::max();
#ifdef _WIN32
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (!GlobalMemoryStatusEx(&memory)) return {};
        available = memory.ullAvailPageFile;
#endif
        auto data = ReadBoundedDriverCache([&](size_t* size, void* bytes) {
            return static_cast<int>(device.getPipelineCacheData(cache, size, bytes));
        }, available);
        if (data.empty()) {
            LOG_WARNING(Render_Vulkan, "CACHE-SAVE-DEFER: no usable bounded driver snapshot; keeping previous cache");
        }
        return data;
    } catch (const std::bad_alloc&) {
        LOG_WARNING(Render_Vulkan, "CACHE-SAVE-DEFER: bounded snapshot allocation failed; keeping previous cache");
        return {};
    }
}


void PipelineCache::SaveDriverCache(bool wait) {
    // FIX-GT7-050: wait even when a rejected cache or empty path prevents a new save.
    if (wait && driver_cache_writer.joinable()) driver_cache_writer.join();
    if (rejected_driver_cache.load() || driver_cache_path.empty() || !pipeline_cache) {
        return;
    }
    // PERF-036: GT Sport's driver cache grows to several GB (3.4 GB on October 9). Reading it out
    // with vkGetPipelineCacheData took about 1.1 s on the GPU thread every 30 s, a frozen frame
    // each time. The read and the write now run on their own thread; a save still running is
    // not waited for, the next one comes later. Pipeline caches are internally synchronized, so
    // builds go on meanwhile. -DisablePerf 51 reads it here as before.
    static const bool off_thread = Common::PerfFeatureEnabled(51);
    if (!wait && driver_cache_writing.load()) {
        return;
    }
    if (driver_cache_writer.joinable()) {
        driver_cache_writer.join();
    }
    pipelines_at_driver_save = num_new_pipelines;
    driver_cache_saved_at = std::chrono::steady_clock::now();
    const auto write = [path = driver_cache_path](std::vector<u8> data) {
        // Write beside the file and rename, so an interrupted write never leaves a broken cache.
        auto temp = path;
        temp += ".tmp";
        if (!WriteVerifiedDriverCacheFile(temp, data)) {
            LOG_WARNING(Render_Vulkan, "CACHE-SAVE-DEFER: cache write, durable flush or verification failed; keeping previous cache");
            return false;
        }
#ifdef _WIN32
        const bool replaced = MoveFileExW(temp.c_str(), path.c_str(),
                                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        const bool replaced = !ec;
#endif
        if (!replaced) LOG_WARNING(Render_Vulkan, "CACHE-SAVE-DEFER: cache replacement failed; keeping previous cache");
        if (replaced) LOG_INFO(Render_Vulkan,
            "CACHE-SAVED: {} bytes, hash {:016x}, file {}", data.size(),
            XXH3_64bits(data.data(), data.size()), path.string());
        return replaced;
    };
    const vk::Device device = instance.GetDevice();
    const vk::PipelineCache cache = *pipeline_cache;
    if (off_thread) {
        driver_cache_writing = true;
        driver_cache_writer = std::jthread([this, device, cache, write] {
            SCOPE_EXIT {
                driver_cache_writing = false;
            };
            try {
                Common::RecoverableScope recoverable;
                Common::SetCurrentThreadName("shadGT:DriverCacheSave");
                const auto start = std::chrono::steady_clock::now();
                auto data = ReadDriverCacheSnapshot(device, cache);
                const auto read_end = std::chrono::steady_clock::now();
                if (!data.empty()) {
                    const size_t size = data.size();
                    if (!write(std::move(data))) return;
                    LOG_WARNING(Render_Vulkan,
                                "PERF-036: driver pipeline cache saved off the GPU thread: {} MB, "
                                "read {:.0f} ms, written {:.0f} ms",
                                size >> 20,
                                std::chrono::duration<double, std::milli>(read_end - start).count(),
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - read_end)
                                    .count());
                }
            } catch (const std::exception& error) {
                LOG_WARNING(Render_Vulkan, "CACHE-SAVE-DEFER: cache save failed: {}", error.what());
            }
        });
    } else {
        auto data = ReadDriverCacheSnapshot(device, cache);
        if (data.empty()) {
            return;
        }
        driver_cache_writing = true;
        driver_cache_writer = std::jthread([this, write, data = std::move(data)]() mutable {
            SCOPE_EXIT {
                driver_cache_writing = false;
            };
            try {
                Common::RecoverableScope recoverable;
                write(std::move(data));
            } catch (const std::exception& error) {
                LOG_WARNING(Render_Vulkan, "CACHE-SAVE-DEFER: cache save failed: {}", error.what());
            }
        });
    }
    if (wait) {
        driver_cache_writer.join();
    }
}

void PipelineCache::MaybeSaveDriverCache() {
#ifdef RYTY_GPU
    // FIX-165: never during play (the whole cache is read out and written each time).
    return;
#endif
    // New pipelines are compiled during play; save them in batches without blocking the frame.
    using namespace std::chrono_literals;
    if (num_new_pipelines - pipelines_at_driver_save >= 64 &&
        std::chrono::steady_clock::now() - driver_cache_saved_at >= 30s) {
        SaveDriverCache(false);
    }
}

const AmdGpu::Regs& PipelineCache::Regs() const {
    return regs_override ? *regs_override : liverpool->DrawRegs();
}

bool PipelineCache::HasSupportedColorTargets() const {
    if (S().graphics_key.num_color_attachments > AmdGpu::NUM_COLOR_BUFFERS) {
        return false;
    }
    for (u32 cb = 0; cb < S().graphics_key.num_color_attachments; ++cb) {
        const auto& color_buffer = S().graphics_key.color_buffers[cb];
        if (color_buffer.data_format != AmdGpu::DataFormat::FormatInvalid &&
            Vulkan::LiverpoolToVK::TrySurfaceFormat(
                color_buffer.data_format, color_buffer.num_format) == vk::Format::eUndefined) {
            return false;
        }
    }
    return true;
}

std::shared_ptr<PipelineCache::PipelineBuild> PipelineCache::MakePipelineBuild() {
    auto build = std::make_shared<PipelineBuild>();
    build->key = S().graphics_key;
    for (size_t stage = 0; stage < MaxShaderStages; ++stage) {
        if (!S().infos[stage]) {
            continue;
        }
        auto& copy = build->info_copies[stage];
        copy = *S().infos[stage];
        build->user_data[stage].assign(copy.user_data.begin(), copy.user_data.end());
        copy.user_data = build->user_data[stage];
        build->infos[stage] = &copy;
        build->canonical[stage] = S().infos[stage];
    }
    build->runtime_infos = S().runtime_infos;
    if (S().fetch_shader) {
        build->fetch = *S().fetch_shader;
    }
    build->modules = S().modules;
    build->done = build->promise.get_future().share();
    build->queued = std::chrono::steady_clock::now();
    return build;
}

void PipelineCache::FinishBuild(PipelineBuild& build) {
    if (!build.started.exchange(true)) {
      const bool foreground = build.urgent.load(std::memory_order_acquire);
      if (foreground) Common::BeginForegroundCompile();
      SCOPE_EXIT { if (foreground) Common::EndForegroundCompile(); };
      try {
        build.pipeline = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, build.key, *pipeline_cache, build.infos,
            build.runtime_infos, build.fetch ? &*build.fetch : nullptr, build.modules, build.sdata,
            build.preloading);
        // The copies were only needed while building; draws use the cached infos.
        build.pipeline->SetStageInfos(build.canonical);
        build.info_copies = {};
        build.user_data = {};
        build.promise.set_value();
      } catch (...) {
        // Publish failures as well as success: no stranded waiter or exception escaping a
        // worker thread. The command thread observes the exception through done.get().
        build.promise.set_exception(std::current_exception());
      }
    }
}

void PipelineCache::NoteAsyncSkip(const std::shared_ptr<PipelineBuild>& build, bool again) {
    catch_up_skips.fetch_add(1, std::memory_order_relaxed);
    build->skips.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    build->last_skip_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(now).count(), std::memory_order_relaxed);
    if (again && build_workers && !build->started.load(std::memory_order_acquire) &&
        !build->bumped.exchange(true)) {
        try {
            { std::scoped_lock lock{on_screen_mutex}; on_screen.push_back(build); }
            // The chosen build is not known until this job runs, so reserve the heavy slot.
            build_workers->Push([this] { BuildOnScreen(); }, BuildQueue::Kind::Screen, true);
        } catch (const std::exception&) {
            std::scoped_lock lock{on_screen_mutex};
            std::erase(on_screen, build);
            build->bumped.store(false, std::memory_order_release);
        }
    }
}

void PipelineCache::BuildOnScreen() {
    std::shared_ptr<PipelineBuild> best;
    {
        std::scoped_lock lock{on_screen_mutex};
        std::erase_if(on_screen, [](const auto& build) { return build->started.load(std::memory_order_acquire); });
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        auto chosen = on_screen.end();
        std::pair<bool, u64> best_score{};
        for (auto it = on_screen.begin(); it != on_screen.end(); ++it) {
            const auto score = std::pair{now - (*it)->last_skip_ms.load(std::memory_order_relaxed) <= 1000,
                                         (*it)->skips.load(std::memory_order_relaxed)};
            if (chosen == on_screen.end() || score > best_score) { chosen = it; best_score = score; }
        }
        if (chosen != on_screen.end()) { best = std::move(*chosen); on_screen.erase(chosen); }
    }
    if (best) FinishBuild(*best);
}

void PipelineCache::UpdateCatchUp(std::chrono::steady_clock::time_point now) {
    static const bool enabled = [] { const char* v = std::getenv("RYGT_COMPILE_CATCH_UP"); return !(v && v[0] == '0'); }();
    static const bool paced = [] { const char* v = std::getenv("RYGT_CATCH_UP_PACE"); return !(v && v[0] == '0'); }();
    if (!enabled || !build_workers) return;
    std::unique_lock lock{catch_up_mutex, std::try_to_lock};
    if (!lock.owns_lock()) return;
    auto& state = catch_up;
    if (state.window == std::chrono::steady_clock::time_point{}) { state.window = now; return; }
    const auto elapsed = now - state.window;
    if (elapsed < std::chrono::milliseconds{250}) return;
    const double rate = double(catch_up_skips.exchange(0, std::memory_order_relaxed)) / std::chrono::duration<double>(elapsed).count();
    state.window = now;
    if (!state.on && rate >= 3000.0) {
        state.calm = 0; state.stepped_back = false;
        state.on = build_workers->SetCatchUp(true);
    } else if (state.on) {
        state.calm = rate < 1000.0 ? state.calm + 1 : 0;
        if (state.calm >= 4) {
            state.on = false;
            if (!state.stepped_back) build_workers->SetCatchUp(false);
        }
    }
    if (!paced) return;
    const auto totals = Common::GetGameFrameTotals();
    if (state.pace_window == std::chrono::steady_clock::time_point{}) {
        state.pace_window = now; state.frames = totals.frames; state.us = totals.us;
        state.pace_limit = PipelineBuildWorkers::UsualWidth();
        return;
    }
    if (now - state.pace_window < std::chrono::seconds{1}) return;
    const u64 frames = totals.frames - state.frames;
    const double ms = frames ? double(totals.us - state.us) / 1000.0 / double(frames)
                             : std::chrono::duration<double, std::milli>(now - state.pace_window).count();
    state.pace_window = now; state.frames = totals.frames; state.us = totals.us;
    if (ms >= 80.0) { state.fast = 0; return; }
    if (ms > 34.0) {
        state.fast = 0;
        if (state.on && !state.stepped_back) {
            state.stepped_back = true;
            build_workers->SetCatchUp(false);
        } else if (state.pace_limit > 1 && build_workers->Busy()) {
            build_workers->SetPace(--state.pace_limit);
        }
    } else if (ms < 33.8 && ++state.fast >= 3) {
        state.fast = 0;
        const u32 usual = PipelineBuildWorkers::UsualWidth();
        if (state.pace_limit < usual) {
            ++state.pace_limit;
            build_workers->SetPace(state.pace_limit == usual ? UINT32_MAX : state.pace_limit);
        } else if (state.on && state.stepped_back) {
            if (build_workers->SetCatchUp(true)) state.stepped_back = false;
        }
    } else if (ms >= 33.8) state.fast = 0;
}

void PipelineCache::QueueBuild(const std::shared_ptr<PipelineBuild>& build, BuildQueue::Kind kind) {
    if (!build_workers) {
        build_workers = std::make_unique<PipelineBuildWorkers>();
    }
    auto job = [this, build] {
        if (!build->started.load()) {
            FinishBuild(*build);
        }
    };
    build_workers->Push(std::move(job), kind,
        Common::PerfFeatureEnabled(79) &&
        QueueWholeTessellation(build->infos[u32(Shader::SwStage::TessellationControl)] != nullptr,
            build->preloading, PrecompileHoldsGame(),
            SharedPipelineLibrariesEnabled(instance.IsGraphicsPipelineLibrarySupported(), build->preloading)));
}

std::shared_ptr<PipelineCache::PipelineBuild> PipelineCache::StartPipelineBuild(bool urgent, bool in_order) {
    auto build = MakePipelineBuild();
    build->urgent = urgent && !in_order;
    QueueBuild(build, !urgent ? BuildQueue::Kind::Normal
                            : in_order ? BuildQueue::Kind::Soon : BuildQueue::Kind::Urgent);
    return build;
}

// PB-1: one stored shader translated again outside the game, from its program's bytes and the
// shader record. False when it could not be translated.
bool ReplayTranslate(const Shader::Profile& profile, Shader::Pools& pools,
                     const std::vector<u8>& program, const Shader::Info& stored,
                     const Shader::StageSpecialization& spec, std::vector<u32>& spv,
                     const std::function<void(const Shader::Info&)>& inspect = {}) {
    if (program.size() < 64 || stored.flattened_ud_buf.size() < 16) {
        return false;
    }
    const u8* footer = program.data() + program.size() - 32;
    u32 bits;
    std::memcpy(&bits, footer + 8, 4);
    const u32 length = bits >> 8;
    if (length == 0 || length > program.size() - 32) {
        return false;
    }
    std::vector<u32> code((program.size() + 3) / 4);
    std::memcpy(code.data(), program.data(), program.size());
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    std::copy_n(stored.flattened_ud_buf.begin(), user_data.size(), user_data.begin());
    const Shader::ShaderParams params{
        .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{user_data},
        .code = std::span<const u32>{code.data(), length / sizeof(u32)},
        .hash = stored.pgm_hash,
    };
    Shader::Info info(stored.hw_stage, stored.sw_stage, params);
    // The vertex attribute V#s, from what the record kept of them (number class, swizzle).
    std::vector<std::array<u32, 4>> sharps;
    for (const auto& attrib : spec.vs_attribs) {
        AmdGpu::Buffer buffer{};
        buffer.base_address = 0x1000;
        buffer.num_records = 1;
        buffer.data_format = 14; // 32_32_32_32
        buffer.num_format = attrib.num_class == AmdGpu::NumberClass::Sint   ? 5
                            : attrib.num_class == AmdGpu::NumberClass::Uint ? 4
                                                                            : 7;
        buffer.dst_sel_x = u32(attrib.dst_select.r);
        buffer.dst_sel_y = u32(attrib.dst_select.g);
        buffer.dst_sel_z = u32(attrib.dst_select.b);
        buffer.dst_sel_w = u32(attrib.dst_select.a);
        auto& out = sharps.emplace_back();
        std::memcpy(out.data(), &buffer, sizeof(buffer));
    }
    // Tessellation constants: the parts translation uses are in the stored runtime info.
    Shader::TessellationDataConstantBuffer tess{};
    if (stored.sw_stage == Shader::SwStage::TessellationControl) {
        tess.ls_stride = spec.runtime_info.sw.tcs.ls_stride;
        tess.hs_cp_stride = spec.runtime_info.sw.tcs.hs_output_cp_stride;
        tess.hs_output_base = spec.runtime_info.sw.tcs.hs_output_base;
    } else if (stored.sw_stage == Shader::SwStage::TessellationEval) {
        tess.hs_cp_stride = spec.runtime_info.sw.tes.hs_output_cp_stride;
    }
    const Shader::ReplayInputs replay{
        .flat = &stored.flattened_ud_buf,
        .fetch = spec.fetch_shader_data.Empty() ? nullptr : &spec.fetch_shader_data,
        .attrib_sharps = &sharps,
        .tess_constants = &tess,
    };
    Shader::t_replay = &replay;
    struct Reset {
        ~Reset() {
            Shader::t_replay = nullptr;
        }
    } reset;
    auto runtime_info = spec.runtime_info;
    auto binding = spec.start;
#ifndef NDEBUG
    Shader::SrtArenaTrace::OriginScope arena_origin{Shader::SrtArenaTrace::Origin::CachedRebuild};
#endif
    try {
        Common::RecoverableScope recoverable;
        const auto ir = Shader::TranslateProgram(params.code, pools, info, runtime_info, profile);
        spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir, binding);
        if (inspect) {
            // PB-4: the translation's resources (their sharps come from the record's user data).
            inspect(info);
        }
    } catch (...) {
        return false;
    }
    return true;
}

// PB-1: shader records without their SPIR-V (put in the store by RyGT's Set up, with the
// programs they were made from in <cache>\<serial>-programs) are translated here, on this PC,
// before the stored pipelines are built.
void PipelineCache::RebuildFromPrograms() {
    const auto& serial = Common::ElfInfo::Instance().GameSerial();
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
                     (std::string{serial} + "-programs");
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return;
    }
    auto& store = Storage::DataBase::Instance();
    const auto start = std::chrono::steady_clock::now();
    const auto metas = store.ListBlobs(Storage::BlobType::ShaderMeta);
    std::atomic<u64> rebuilt = 0, failed = 0, no_program = 0;
    std::atomic<size_t> next = 0;
    // PERF-161: translated on several threads (a first boot with RyGT's shader pack translated
    // 12,590 shaders in 36 s on one). -DisablePerf 161 uses one.
    static const bool parallel = Common::PerfFeatureEnabled(161);
    const u32 threads =
        parallel ? CompilerWorkerBudget::StartupPipelineWorkers(std::thread::hardware_concurrency()) : 1u;
    const auto work = [&] {
        Shader::Pools pools;
        for (size_t i = next++; i < metas.size(); i = next++) {
            const auto& name = metas[i].first;
            std::vector<u8> blob;
            store.Load(Storage::BlobType::ShaderMeta, name, blob);
            Shader::Info info{};
            Shader::StageSpecialization spec{};
            spec.info = &info;
            size_t perm_idx{};
            Serialization::Archive ar{std::move(blob)};
            try {
                Common::RecoverableScope recoverable;
                if (ar.SizeBytes() < 24 || !LoadShaderMeta(ar, info, spec, perm_idx)) {
                    continue;
                }
            } catch (...) {
                continue;
            }
            const auto spv_name = fmt::format("{:#018x}_{}", info.pgm_hash, perm_idx);
            if (store.Exists(Storage::BlobType::ShaderBinary, spv_name)) {
                continue;
            }
            std::vector<u8> program;
            if (Common::FS::IOFile file{dir / fmt::format("{:016x}.bin", info.pgm_hash),
                                        Common::FS::FileAccessMode::Read};
                file.IsOpen()) {
                program.resize(file.GetSize());
                file.ReadSpan(std::span{program});
            }
            if (program.empty()) {
                ++no_program;
                continue;
            }
            std::vector<u32> spv;
            if (!ReplayTranslate(profile, pools, program, info, spec, spv)) {
                ++failed;
                continue;
            }
            RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);
            ++rebuilt;
        }
    };
    {
        std::vector<std::jthread> workers;
        for (u32 t = 1; t < threads; ++t) {
            workers.emplace_back([&] {
                Common::SetCurrentThreadName("shadGT:PackTranslate");
                work();
            });
        }
        work();
    }
    if (rebuilt + failed + no_program != 0) {
        LOG_WARNING(Render_Vulkan,
                    "PB-1: {} shaders translated from this PC's game files in {:.1f} s on {} "
                    "threads ({} failed, {} without their program)",
                    rebuilt.load(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                    threads, failed.load(), no_program.load());
    }
}

// PB-2 (SHADGT_TEMPLATE_TEST=<folder>): for the programs in <folder>\unseen.txt, how many of
// their stored records have the same specialization as a record of another program (a
// "template" an unseen program could borrow), and for pipelines of two unseen programs, how far
// apart the vertex and fragment programs sit in their file (<folder>\slots.tsv: hash, path,
// slot). Results in the log.
void PipelineCache::TemplateTest(const std::string& dir) {
    Common::SetCurrentThreadName("shadGT:TemplateTest");
    std::unordered_set<u64> unseen;
    {
        std::ifstream in{std::filesystem::path{dir} / "unseen.txt"};
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) unseen.insert(std::stoull(line, nullptr, 16));
        }
    }
    std::unordered_map<u64, std::pair<std::string, int>> slots;
    {
        std::ifstream in{std::filesystem::path{dir} / "slots.tsv"};
        std::string h, path, slot;
        while (std::getline(in, h, '\t') && std::getline(in, path, '\t') && std::getline(in, slot)) {
            slots[std::stoull(h, nullptr, 16)] = {path, std::stoi(slot)};
        }
    }
    struct Record {
        u64 pgm;
        std::unique_ptr<Shader::Info> info;
        Shader::StageSpecialization spec;
    };
    std::vector<Record> records;
    std::unordered_map<u64, u64> perm_to_pgm;
    auto& store = Storage::DataBase::Instance();
    for (const auto& [name, time] : store.ListBlobs(Storage::BlobType::ShaderMeta)) {
        std::vector<u8> blob;
        store.Load(Storage::BlobType::ShaderMeta, name, blob);
        Record r;
        r.info = std::make_unique<Shader::Info>();
        r.spec.info = r.info.get();
        size_t perm_idx{};
        Serialization::Archive ar{std::move(blob)};
        try {
            Common::RecoverableScope recoverable;
            if (ar.SizeBytes() < 24 || !LoadShaderMeta(ar, *r.info, r.spec, perm_idx)) continue;
        } catch (...) {
            continue;
        }
        r.spec.info = r.info.get();
        r.pgm = r.info->pgm_hash;
        perm_to_pgm[std::stoull(name, nullptr, 16)] = r.pgm;
        records.push_back(std::move(r));
    }
    u64 tried = 0, matched = 0, by_stage_tried[8]{}, by_stage_matched[8]{};
    std::map<std::string, u64> reasons;
    for (const auto& p : records) {
        if (!unseen.contains(p.pgm)) continue;
        ++tried;
        const u32 stage = std::min<u32>(u32(p.info->sw_stage), 7);
        ++by_stage_tried[stage];
        bool found = false;
        const char* best_reason = "no record of that stage";
        for (const auto& t : records) {
            if (unseen.contains(t.pgm) || t.info->sw_stage != p.info->sw_stage) continue;
            if (t.spec == p.spec) {
                found = true;
                break;
            }
            u32 index{};
            if (const char* why = t.spec.FirstDifference(p.spec, index)) best_reason = why;
        }
        if (found) {
            ++matched;
            ++by_stage_matched[stage];
        } else {
            ++reasons[best_reason];
        }
    }
    std::string why_text;
    for (const auto& [why, n] : reasons) why_text += fmt::format(" {}={}", why, n);
    LOG_WARNING(Render_Vulkan,
                "PB-2 templates: {} records of unseen programs, {} have the specialization of a "
                "record of another program; by stage (same/all) fs {}/{} vs {}/{} cs {}/{}; last "
                "difference of the others:{}",
                tried, matched, by_stage_matched[0], by_stage_tried[0], by_stage_matched[3],
                by_stage_tried[3], by_stage_matched[5], by_stage_tried[5], why_text);
    // Vertex/fragment pairs of unseen programs: their distance in the file.
    std::map<int, u64> distance;
    u64 pairs = 0, other_file = 0, no_slot = 0, fs_unseen_pipelines = 0, fs_unseen_vs_known = 0;
    std::unordered_map<u64, u64> known_vs;
    for (const auto& [name, time] : store.ListBlobs(Storage::BlobType::PipelineKey)) {
        if (!name.starts_with("g_")) continue;
        std::vector<u8> blob;
        store.Load(Storage::BlobType::PipelineKey, name, blob);
        GraphicsPipelineKey key{};
        GraphicsPipeline::SerializationSupport sdata{};
        try {
            Common::RecoverableScope recoverable;
            if (!LoadStoredGraphicsPipeline(std::move(blob), key, sdata)) continue;
        } catch (...) {
            continue;
        }
        const auto vs = perm_to_pgm.find(key.stage_hashes[u32(Shader::SwStage::Vertex)]);
        const auto fs = perm_to_pgm.find(key.stage_hashes[u32(Shader::SwStage::Fragment)]);
        if (vs != perm_to_pgm.end() && fs != perm_to_pgm.end() && unseen.contains(fs->second)) {
            ++fs_unseen_pipelines;
            if (!unseen.contains(vs->second)) {
                ++fs_unseen_vs_known;
                known_vs[vs->second]++;
            }
        }
        if (vs == perm_to_pgm.end() || fs == perm_to_pgm.end() || !unseen.contains(vs->second) ||
            !unseen.contains(fs->second)) {
            continue;
        }
        ++pairs;
        const auto a = slots.find(vs->second);
        const auto b = slots.find(fs->second);
        if (a == slots.end() || b == slots.end()) {
            ++no_slot;
        } else if (a->second.first != b->second.first) {
            ++other_file;
        } else {
            ++distance[std::clamp(b->second.second - a->second.second, -9, 9)];
        }
    }
    std::string dist_text;
    for (const auto& [d, n] : distance) dist_text += fmt::format(" {}:{}", d, n);
    LOG_WARNING(Render_Vulkan,
                "PB-2 pairs: {} pipelines of two unseen programs; {} in different files, {} "
                "without a slot; fragment slot minus vertex slot (count):{}",
                pairs, other_file, no_slot, dist_text);
    // Simulation of the loading-screen rule: a pipeline of an unseen fragment program P and a
    // known vertex permutation V is predicted when a known pipeline with the same V and the same
    // state (everything but the fragment stage) has a fragment record whose specialization is
    // P's. Counts how many of the real pipelines would have been predicted, and how many
    // candidates the rule makes.
    {
        std::unordered_map<u64, const Record*> rec_by_perm;
        for (const auto& [perm, pgm] : perm_to_pgm) {
            (void)pgm;
        }
        // perm hash -> record (records are in ListBlobs order; rebuild a map from names)
        std::unordered_map<u64, size_t> perm_index;
        {
            size_t i = 0;
            for (const auto& [name, time] : store.ListBlobs(Storage::BlobType::ShaderMeta)) {
                (void)time;
                if (i < records.size()) {
                    perm_index[std::stoull(name, nullptr, 16)] = i;
                }
                ++i;
            }
        }
        struct Known {
            GraphicsPipelineKey key;
        };
        std::unordered_map<u64, std::vector<GraphicsPipelineKey>> known_by_vs;
        std::vector<GraphicsPipelineKey> wanted;
        const u32 vs_i = u32(Shader::SwStage::Vertex), fs_i = u32(Shader::SwStage::Fragment);
        for (const auto& [name, time] : store.ListBlobs(Storage::BlobType::PipelineKey)) {
            if (!name.starts_with("g_")) continue;
            std::vector<u8> blob;
            store.Load(Storage::BlobType::PipelineKey, name, blob);
            GraphicsPipelineKey key{};
            GraphicsPipeline::SerializationSupport sdata{};
            try {
                Common::RecoverableScope recoverable;
                if (!LoadStoredGraphicsPipeline(std::move(blob), key, sdata)) continue;
            } catch (...) {
                continue;
            }
            const auto vs = perm_to_pgm.find(key.stage_hashes[vs_i]);
            const auto fs = perm_to_pgm.find(key.stage_hashes[fs_i]);
            if (vs == perm_to_pgm.end() || fs == perm_to_pgm.end() || unseen.contains(vs->second)) {
                continue;
            }
            if (unseen.contains(fs->second)) {
                wanted.push_back(key);
            } else {
                known_by_vs[key.stage_hashes[vs_i]].push_back(key);
            }
        }
        u64 predicted = 0, candidates = 0;
        for (const auto& want : wanted) {
            const auto it = known_by_vs.find(want.stage_hashes[vs_i]);
            if (it == known_by_vs.end()) continue;
            const auto pi = perm_index.find(want.stage_hashes[fs_i]);
            if (pi == perm_index.end() || pi->second >= records.size()) continue;
            const auto& p_spec = records[pi->second].spec;
            bool hit = false;
            for (const auto& known : it->second) {
                GraphicsPipelineKey a = want, b = known;
                a.stage_hashes[fs_i] = 0;
                b.stage_hashes[fs_i] = 0;
                if (!(a == b)) continue;
                ++candidates;
                const auto ti = perm_index.find(known.stage_hashes[fs_i]);
                if (ti != perm_index.end() && ti->second < records.size() &&
                    records[ti->second].spec == p_spec) {
                    hit = true;
                }
            }
            predicted += hit ? 1 : 0;
        }
        LOG_WARNING(Render_Vulkan,
                    "PB-2 prediction: {} pipelines of unseen fragment and known vertex programs; "
                    "{} predicted by a known pipeline with the same vertex permutation and state "
                    "and an equal fragment specialization ({} same-state candidates in all)",
                    wanted.size(), predicted, candidates);
    }
    std::vector<std::pair<u64, u64>> top(known_vs.begin(), known_vs.end());
    std::ranges::sort(top, [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string top_text;
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
        const auto s = slots.find(top[i].first);
        top_text += fmt::format(" {:#x}x{} ({})", top[i].first, top[i].second,
                                s != slots.end() ? s->second.first : "not in the files");
    }
    LOG_WARNING(Render_Vulkan,
                "PB-2 unseen fragment shaders: {} pipelines, {} with a vertex shader seen before "
                "({} different ones); most used:{}",
                fs_unseen_pipelines, fs_unseen_vs_known, known_vs.size(), top_text);
}

void PipelineCache::ReplayTest(const std::string& dir) {
    Common::SetCurrentThreadName("shadGT:ReplayTest");
    auto& store = Storage::DataBase::Instance();
    const auto metas = store.ListBlobs(Storage::BlobType::ShaderMeta);
    Shader::Pools pools;
    u64 tested = 0, same = 0, differ = 0, failed = 0, no_program = 0, by_stage_same[8]{}, by_stage_all[8]{};
    std::vector<std::string> identical, not_identical;
    for (const auto& [name, time] : metas) {
        std::vector<u8> blob;
        store.Load(Storage::BlobType::ShaderMeta, name, blob);
        Shader::Info info{};
        Shader::StageSpecialization spec{};
        spec.info = &info;
        size_t perm_idx{};
        Serialization::Archive ar{std::move(blob)};
        try {
            Common::RecoverableScope recoverable;
            if (ar.SizeBytes() < 24 || !LoadShaderMeta(ar, info, spec, perm_idx)) {
                continue;
            }
        } catch (...) {
            continue;
        }
        const auto path = std::filesystem::path{dir} / fmt::format("{:016x}.bin", info.pgm_hash);
        std::vector<u8> program;
        if (Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read}; file.IsOpen()) {
            program.resize(file.GetSize());
            file.ReadSpan(std::span{program});
        }
        if (program.empty()) {
            ++no_program;
            not_identical.push_back(fmt::format("{}\t{:016x}", name, info.pgm_hash));
            continue;
        }
        std::vector<u32> stored_spv;
        store.Load(Storage::BlobType::ShaderBinary, fmt::format("{:#018x}_{}", info.pgm_hash, perm_idx),
                   stored_spv);
        std::vector<u32> spv;
        ++tested;
        const u32 stage = std::min<u32>(u32(info.sw_stage), 7);
        ++by_stage_all[stage];
        if (!ReplayTranslate(profile, pools, program, info, spec, spv)) {
            ++failed;
            not_identical.push_back(fmt::format("{}\t{:016x}", name, info.pgm_hash));
        } else if (spv == stored_spv) {
            ++same;
            ++by_stage_same[stage];
            identical.push_back(fmt::format("{}\t{:016x}", name, info.pgm_hash));
        } else {
            ++differ;
            not_identical.push_back(fmt::format("{}\t{:016x}", name, info.pgm_hash));
            if (differ <= 10) {
                LOG_WARNING(Render_Vulkan, "PB-1: {} differs (stage {}, {} vs {} words)", name,
                            stage, spv.size(), stored_spv.size());
            }
        }
    }
    LOG_WARNING(Render_Vulkan,
                "PB-1 replay test: {} shaders tested, {} identical, {} different, {} failed, {} "
                "without a program file; identical by stage (sw stage: same/all): {}/{} {}/{} {}/{} "
                "{}/{} {}/{} {}/{}",
                tested, same, differ, failed, no_program, by_stage_same[0], by_stage_all[0],
                by_stage_same[1], by_stage_all[1], by_stage_same[2], by_stage_all[2],
                by_stage_same[3], by_stage_all[3], by_stage_same[4], by_stage_all[4],
                by_stage_same[5], by_stage_all[5]);
    // The records that translate back exactly, for RyGT's shader pack.
    std::ofstream list{Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "replay_identical.txt"};
    for (const auto& name : identical) {
        list << name << '\n';
    }
    // The others: a program is shipped only with all of its permutations (their indices are
    // the store's, and a gap made the renderer build one pipeline again and again).
    std::ofstream other{Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "replay_other.txt"};
    for (const auto& name : not_identical) {
        other << name << '\n';
    }
}

// PB-4 (SHADGT_DUMP_STORE=<folder>): writes the store's shader records and pipeline keys as
// text, for research on predicting them from the game's files: <folder>\metas.tsv (each record
// translated again from <folder>\programs\<hash>.bin, with every resource's sharp read from the
// record's user data) and <folder>\keys.tsv.
void PipelineCache::DumpStore(const std::string& dir) {
    Common::SetCurrentThreadName("shadGT:DumpStore");
    const auto hex = [](const void* data, size_t size) {
        std::string out;
        const auto* bytes = static_cast<const u8*>(data);
        for (size_t i = 0; i < size; ++i) {
            out += fmt::format("{:02x}", bytes[i]);
        }
        return out;
    };
    auto& store = Storage::DataBase::Instance();
    Shader::Pools pools;
    u64 written = 0, no_program = 0, failed = 0;
    {
        std::ofstream out{std::filesystem::path{dir} / "metas.tsv"};
        out << "name\tpgm\tperm\tstage\tflat_dw\truntime\tvs_attribs\tfetch\tbuffers\timages\tsamplers"
               "\tspec_buffers\tspec_images\tspec_samplers\n";
        for (const auto& [name, time] : store.ListBlobs(Storage::BlobType::ShaderMeta)) {
            std::vector<u8> blob;
            store.Load(Storage::BlobType::ShaderMeta, name, blob);
            Shader::Info stored{};
            Shader::StageSpecialization spec{};
            spec.info = &stored;
            size_t perm_idx{};
            Serialization::Archive ar{std::move(blob)};
            try {
                Common::RecoverableScope recoverable;
                if (ar.SizeBytes() < 24 || !LoadShaderMeta(ar, stored, spec, perm_idx)) continue;
            } catch (...) {
                continue;
            }
            std::string runtime;
            const auto& rt = spec.runtime_info;
            if (stored.sw_stage == Shader::SwStage::Fragment) {
                const auto& fs = rt.hw.fs;
                runtime = fmt::format("fs en={} addr={} inputs={} [",
                                      hex(&fs.en_flags, sizeof(fs.en_flags)),
                                      hex(&fs.addr_flags, sizeof(fs.addr_flags)), fs.num_inputs);
                for (u32 i = 0; i < fs.num_inputs && i < fs.inputs.size(); ++i) {
                    const auto& in = fs.inputs[i];
                    runtime += fmt::format("{}{}{}{},", in.param_index, in.is_default ? "d" : "",
                                           in.is_flat ? "f" : "", in.is_default ? in.default_value : 0);
                }
                runtime += "] cb=[";
                for (const auto& cb : fs.color_buffers) {
                    runtime += fmt::format("{}/{}/{}/{}/{},", u32(cb.data_format), u32(cb.num_format),
                                           u32(cb.num_conversion), u32(cb.export_format),
                                           hex(&cb.swizzle, sizeof(cb.swizzle)));
                }
                runtime += fmt::format("] z={} samples={} mrtz={} dual={}", u32(fs.z_export_format),
                                       fs.num_samples, fs.mrtz_mask, fs.dual_source_blending);
            } else if (stored.hw_stage == Shader::HwStage::Vertex) {
                const auto& vs = rt.hw.vs;
                runtime = fmt::format("vs outputs={} map={} clip_disable={} ucp={}", vs.num_outputs,
                                      hex(vs.outputs.data(), sizeof(vs.outputs)), vs.clip_disable,
                                      vs.user_clip_plane_mask);
            } else {
                runtime = fmt::format("stage{} {}", u32(stored.hw_stage), hex(&rt, sizeof(rt)));
            }
            runtime += fmt::format(" ud={} vgprs={}/{}", rt.props.num_user_data,
                                   rt.props.num_input_vgprs, rt.props.num_allocated_vgprs);
            std::string vs_attribs;
            for (const auto& a : spec.vs_attribs) {
                vs_attribs += fmt::format("{}/{}/{},", a.divisor, u32(a.num_class),
                                          hex(&a.dst_select, sizeof(a.dst_select)));
            }
            std::string fetch;
            for (const auto& a : spec.fetch_shader_data.attributes) {
                fetch += hex(&a, sizeof(a)) + ",";
            }
            std::string spec_buffers, spec_images, spec_samplers;
            for (const auto& b : spec.buffers) {
                spec_buffers += fmt::format("{}/{}/{}/{}/{},", u32(b.stride), u32(b.is_formatted),
                                            u32(b.data_format), u32(b.num_format),
                                            u32(b.swizzle_enable));
            }
            for (const auto& i : spec.images) {
                spec_images += fmt::format("{}/{}/{}/{}/{}/{},", u32(i.type), i.is_integer,
                                           i.is_storage, i.is_srgb, u32(i.num_conversion),
                                           i.num_bindings);
            }
            for (const auto& s : spec.samplers) {
                spec_samplers += fmt::format("{}/{},", u32(s.force_unnormalized), u32(s.force_degamma));
            }
            std::string buffers = "-", images = "-", samplers = "-";
            std::vector<u8> program;
            if (Common::FS::IOFile file{std::filesystem::path{dir} / "programs" /
                                            fmt::format("{:016x}.bin", stored.pgm_hash),
                                        Common::FS::FileAccessMode::Read};
                file.IsOpen()) {
                program.resize(file.GetSize());
                file.ReadSpan(std::span{program});
            }
            if (program.empty()) {
                ++no_program;
            } else {
                std::vector<u32> spv;
                const bool ok = ReplayTranslate(
                    profile, pools, program, stored, spec, spv, [&](const Shader::Info& info) {
                        buffers.clear();
                        images.clear();
                        samplers.clear();
                        for (const auto& b : info.buffers) {
                            const auto sharp = b.GetSharp(info);
                            buffers += fmt::format("{}/{}/{}/{}:{},", u32(b.buffer_type), b.is_written,
                                                   b.is_formatted, u32(b.used_types),
                                                   hex(&sharp, sizeof(sharp)));
                        }
                        for (const auto& i : info.images) {
                            const auto sharp = i.GetSharp(info);
                            images += fmt::format("{}{}{}{}:{},", i.is_depth ? "d" : "",
                                                  i.is_array ? "a" : "", i.is_written ? "w" : "",
                                                  i.is_r128 ? "r" : "", hex(&sharp, sizeof(sharp)));
                        }
                        for (const auto& s : info.samplers) {
                            const auto sharp = s.GetSharp(info);
                            samplers += hex(&sharp, sizeof(sharp)) + ",";
                        }
                    });
                if (!ok) {
                    ++failed;
                }
            }
            out << name << '\t' << fmt::format("{:016x}", stored.pgm_hash) << '\t' << perm_idx << '\t'
                << u32(stored.sw_stage) << '\t' << stored.flattened_ud_buf.size() << '\t' << runtime
                << '\t' << vs_attribs << '\t' << fetch << '\t' << buffers << '\t' << images << '\t'
                << samplers << '\t' << spec_buffers << '\t' << spec_images << '\t' << spec_samplers
                << '\n';
            ++written;
        }
    }
    u64 keys = 0;
    {
        std::ofstream out{std::filesystem::path{dir} / "keys.tsv"};
        out << "name\tstages\tvertex_formats\tcolor\tblend\twrite_masks\tmisc\n";
        for (const auto& [name, time] : store.ListBlobs(Storage::BlobType::PipelineKey)) {
            if (!name.starts_with("g_")) continue;
            std::vector<u8> blob;
            store.Load(Storage::BlobType::PipelineKey, name, blob);
            GraphicsPipelineKey key{};
            GraphicsPipeline::SerializationSupport sdata{};
            try {
                Common::RecoverableScope recoverable;
                if (!LoadStoredGraphicsPipeline(std::move(blob), key, sdata)) continue;
            } catch (...) {
                continue;
            }
            std::string stages, formats, color, blend, masks;
            for (const auto h : key.stage_hashes) stages += fmt::format("{:x},", h);
            for (const auto f : key.vertex_buffer_formats) {
                if (f != vk::Format::eUndefined) formats += fmt::format("{},", u32(f));
            }
            for (u32 i = 0; i < key.num_color_attachments && i < key.color_buffers.size(); ++i) {
                const auto& cb = key.color_buffers[i];
                color += fmt::format("{}/{}/{}/{},", u32(cb.data_format), u32(cb.num_format),
                                     u32(cb.num_conversion), u32(cb.export_format));
                blend += hex(&key.blend_controls[i], sizeof(key.blend_controls[i])) + ",";
                masks += fmt::format("{},", u32(key.write_masks[i]));
            }
            out << name << '\t' << stages << '\t' << formats << '\t' << color << '\t' << blend << '\t'
                << masks << '\t'
                << fmt::format("cbmask={} samples={}/{} mrt={:x} z={} stencil={} prim={} poly={} "
                               "patch={}",
                               hex(&key.cb_shader_mask, sizeof(key.cb_shader_mask)),
                               key.num_samples, key.depth_samples, key.mrt_mask, u32(key.z_format),
                               u32(key.stencil_format), u32(key.prim_type), u32(key.polygon_mode),
                               key.patch_control_points)
                << '\n';
            ++keys;
        }
    }
    LOG_WARNING(Render_Vulkan,
                "PB-4 store dump: {} shader records ({} without a program, {} failed), {} pipeline "
                "keys in {}",
                written, no_program, failed, keys, dir);
}

bool PipelineCache::LegacyWaits() const {
    // Keep going: rendering must not wait for a cosmetic pipeline.
    if (CompileMode() == 2) return false;
    // FIX-119: a pipeline of the old renderer's translations (D10) is built at once, as the
    // driver usually has it already. Not in a race: there one the driver did not have stopped
    // the game for up to seconds (19.7 s of such waits in one test race); it builds in the
    // background like the rest. -DisablePerf 141 waits for them in races too.
    if (!S().key_uses_legacy) {
        return false;
    }
#ifdef RYTY_GPU
    static const bool skip_in_races = Common::PerfFeatureEnabled(141);
    if (skip_in_races && RytyGtsInRace()) {
        return false;
    }
#endif
    return true;
}

std::chrono::microseconds PipelineCache::QuickWait() {
    if (CompileMode() == 2) return std::chrono::microseconds{0};
#ifdef RYTY_GPU
    // Deferrable race draws poll the worker result, rather than spending up to 4 ms per
    // frame waiting. Memory-writing and never-skipped draws still take the required wait.
    if (RytyGtsInRace()) return std::chrono::microseconds{0};
#endif
    // FIX-114: a draw whose new pipeline is building waits for it a little, so the pipelines
    // that build quickly (the driver had them, or small ones) are drawn at once and only the
    // slow ones pop in a moment later. At most 4 ms of waiting in any 16 ms, 2 ms per draw.
    // -DisablePerf 137 skips without waiting.
    static const bool enabled = Common::PerfFeatureEnabled(137);
    if (!enabled) {
        return std::chrono::microseconds{0};
    }
    using namespace std::chrono;
    static thread_local steady_clock::time_point window{};
    static thread_local microseconds used{};
    const auto now = steady_clock::now();
    if (now - window > milliseconds{16}) {
        window = now;
        used = microseconds{0};
    }
    const auto wait = std::clamp(microseconds{4000} - used, microseconds{0}, microseconds{2000});
    used += wait; // charged up front: simple, and errs on the side of fewer waits
    return wait;
}

bool PipelineCache::MenusWait() {
    // Captures retain every producer, including intermediate lighting passes,
    // until their actual save job completes. Ordinary Keep Going menu frames
    // may skip cosmetic work; an unavailable/ambiguous capture hook fails closed.
#ifdef RYTY_GPU
    static const bool logged = [] {
        LOG_INFO(Render_Vulkan, "Compile policy: mode={} (0=pause, 1=slow motion, 2=keep going), capture guard idle={}",
                 CompileMode(), RytyGtsMenuCaptureCanContinue());
        return true;
    }();
    (void)logged;
    return Native::MenuCompileMustWait(CompileMode(), RytyGtsInRace(),
                                       RytyGtsMenuCaptureAvailable(), RytyGtsMenuCaptureCanContinue());
#else
    return false;
#endif
}

bool PipelineCache::CancelBuild(PipelineBuild& build) {
    // A worker may already have taken the job and wait in FinishBuild for whoever claimed the
    // build; release it, the build stays empty.
    return CanCancelRequiredBuild(build.required_waiters) && CancelUnstartedPipeline(build);
}

void PipelineCache::QueueBackgroundBuild(const std::shared_ptr<PipelineBuild>& build) {
    if (!build_workers) {
        build_workers = std::make_unique<PipelineBuildWorkers>();
    }
    build_workers->PushBackground([this, build] {
        if (!build->started.load()) {
            FinishBuild(*build);
#ifndef NDEBUG
            ++prewarm_built;
#endif
        }
        ++g_precompile_done; // PERF-098
    }, Common::PerfFeatureEnabled(79) &&
       QueueWholeTessellation(build->infos[u32(Shader::SwStage::TessellationControl)] != nullptr,
            build->preloading, PrecompileHoldsGame(),
            SharedPipelineLibrariesEnabled(instance.IsGraphicsPipelineLibrarySupported(), build->preloading)));
}

void PipelineCache::ReadAheadAtBufferStart() {
    using namespace std::chrono_literals;
    if (!build_workers || !Common::PerfFeatureEnabled(20) || !Common::PerfFeatureEnabled(19) ||
        std::chrono::steady_clock::now() - last_pipeline_miss > 2s) {
        return;
    }
    // No draw is being processed, so nothing needs restoring afterwards.
    ReadAhead(S().draw_indirect_params, false);
}

void PipelineCache::ReadAhead(const DrawIndirectParams params, bool restore) {
#ifndef NDEBUG
    const auto start = std::chrono::steady_clock::now();
#endif
    u32 draws = 0;
    u32 started = 0;
    u32 evaluated = 0;
    const bool dedupe = Common::PerfFeatureEnabled(21);
    liverpool->ScanAheadForPipelines([&](const AmdGpu::Regs& regs) {
        ++draws;
        // FIX-014: draws the rasterizer filters out (Rasterizer::FilterDraw) never get a
        // pipeline; building one for them can fail (primitive type 'None' asserted on a build
        // worker when selecting a car).
        using OperationMode = AmdGpu::ColorControl::OperationMode;
        const auto mode = regs.color_control.mode;
        if (regs.primitive_type == AmdGpu::PrimitiveType::None ||
            mode == OperationMode::EliminateFastClear || mode == OperationMode::FmaskDecompress ||
            mode == OperationMode::Resolve) {
            return false;
        }
        if (dedupe) {
            // Everything the pipeline key is built from that the commands can change: graphics
            // shader registers (programs and user data), context registers (targets, blending,
            // depth, rasterizer state) and the primitive type.
            const auto& r = regs.reg_array;
            constexpr u32 ShGfxRegs = 0x200;
            constexpr u32 NumContextRegs = 0x400;
            constexpr u32 PrimitiveRegs = 0x100;
            std::array<u32, ShGfxRegs> sh;
            std::memcpy(sh.data(), &r[AmdGpu::Regs::ShRegWordOffset], sizeof(sh));
            if (Common::PerfFeatureEnabled(22)) {
                // PERF-022: user data (per-draw constants and resource pointers) changes on
                // nearly every draw; the programs and the other state decide the pipeline.
                // Each stage's 16 user data registers start 0xC after its program registers.
                for (u32 stage_base = 0; stage_base < ShGfxRegs; stage_base += 0x40) {
                    std::fill_n(sh.begin() + stage_base + 0xC, 16, 0u);
                }
            }
            u64 hash = XXH3_64bits(sh.data(), sizeof(sh));
            hash = XXH3_64bits_withSeed(&r[AmdGpu::Regs::ContextRegWordOffset],
                                        NumContextRegs * sizeof(u32), hash);
            hash = XXH3_64bits_withSeed(&r[AmdGpu::Regs::UconfigRegWordOffset + 0x200],
                                        PrimitiveRegs * sizeof(u32), hash);
            if (!read_ahead_states.insert(hash).second) {
                return false;
            }
            if (read_ahead_states.size() > 1u << 16) {
                read_ahead_states.clear();
            }
        }
        ++evaluated;
        S().draw_indirect_params = {};
        const bool valid = TrySpeculativeKey(regs_override, &regs, [&] { return RefreshGraphicsKey(); });
        if (!valid || !HasSupportedColorTargets() || graphics_pipelines.contains(S().graphics_key)) {
            return false;
        }
        if (pending_builds.contains(S().graphics_key)) {
            return false;
        }
        pending_builds.emplace(S().graphics_key, StartPipelineBuild(false));
        ++started;
        return true;
    });
    if (draws > 0) {
        last_scan_draws = draws;
        draws_since_scan = 0;
        // Back to the draw being processed.
        S().draw_indirect_params = params;
        if (restore && evaluated > 0) {
            RefreshGraphicsKey();
        }
    }
#ifndef NDEBUG
    // One summary every 2 s instead of a line per read.
    auto& stats = read_ahead_stats;
    const auto now = std::chrono::steady_clock::now();
    ++stats.scans;
    stats.draws += draws;
    stats.evaluated += evaluated;
    stats.started += started;
    stats.ms += std::chrono::duration<double, std::milli>(now - start).count();
    if (now - stats.since >= std::chrono::seconds{2}) {
        if (stats.draws > 0) {
            LOG_WARNING(Render_Vulkan,
                        "Pipeline read-ahead in {:.1f} s: {} reads, {} draws read, {} evaluated, "
                        "{} builds started, {:.1f} ms, {} pending",
                        std::chrono::duration<double>(now - stats.since).count(), stats.scans,
                        stats.draws, stats.evaluated, stats.started, stats.ms,
                        pending_builds.size());
        }
        stats = {};
        stats.since = now;
    }
#endif
}

// Required-build context is local to this translation unit and restored after nested waits.
static thread_local const std::shared_future<void>* t_required_pipeline_build = nullptr;
static thread_local std::unique_lock<LookupLock>* t_required_lookup_lock = nullptr;
thread_local std::vector<std::shared_ptr<PipelineCache::TranslateJob>>
    PipelineCache::unlocked_translations;
thread_local std::vector<std::shared_future<PipelineCache::StoredListing>>
    PipelineCache::unlocked_listings;
static bool UnlockedShadersEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("RYGT_ONE_OFF_SHADERS_UNLOCKED");
        return !value || value[0] != '0';
    }();
    return enabled;
}

static bool RetryBlockedReadAhead() {
    static const bool enabled = [] {
        const char* value = std::getenv("RYGT_READ_AHEAD_RETRY");
        return !value || value[0] != '0';
    }();
    return enabled;
}

bool PipelineCache::DirectOnlyRetryProgram(u64 hash) const {
    const auto program = program_cache.find(hash);
    if (program == program_cache.end() || !program->second) return false;
    for (const auto& module : program->second->modules) {
        if (!module.info || !module.spec.fetch_shader_data.Empty() ||
            !DirectOnlyRetryInputs(module.info->srt_info.walker_func != nullptr,
                module.info->has_fetch_shader, bool(module.info->translated_fetch_shader))) return false;
    }
    const auto direct_jobs = [hash](const auto& map) {
        const auto found = map.find(hash);
        if (found == map.end()) return true;
        return std::ranges::all_of(found->second, [](const auto& job) {
            return job->retry_direct_only;
        });
    };
    return direct_jobs(translate_jobs) && direct_jobs(ready_translations);
}

bool PipelineCache::CaptureBlockedAheadInputs(const AmdGpu::Regs& regs, BlockedAheadState& state) {
#ifndef RYTY_GPU
    return false; // No guarded guest reader is available here.
#else
    constexpr size_t MaxProgramBytes = 64 * 1024;
    constexpr size_t MaxStateBytes = 256 * 1024;
    if (!DirectOnlyRetryLayout(regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs))
        return false;
    size_t active = 0;
    state.bytes = state.ranges.capacity() * sizeof(u32);
    for (u32 stage = 0; stage < state.stages.size(); ++stage) {
        if (!regs.stage_enable.IsStageEnabled(stage)) continue;
        const auto* pgm = regs.ProgramForStage(stage);
        if (!pgm || !pgm->Address<VAddr>()) return false;
        const VAddr base = pgm->Address<VAddr>();
        auto program = CaptureShaderSnapshot<AmdGpu::BinaryInfo>(
            base, [](void* dst, uintptr_t address, size_t size) {
                return Core::RytyMemory::GuardedCopy(dst, reinterpret_cast<const void*>(address), size);
            }, MaxProgramBytes);
        if (!program) return false;
        if (!DirectOnlyRetryProgram(program->hash)) return false;
        bool direct_owned_job = false;
        const auto jobs = translate_jobs.find(program->hash);
        if (jobs != translate_jobs.end()) {
            for (const auto& job : jobs->second) {
                if (OwnedRetryJobMatches(stage, base, pgm->user_data, program->code,
                                         u32(job->hw_stage), job->code_base, job->user_data,
                                         job->code_copy, job->retry_direct_only)) {
                    direct_owned_job = true;
                    break;
                }
            }
        }
        // Only direct user data is replayed. A walker, fetch shader or missing
        // prepared stage can follow guest pointers after the state is retained.
        if (!direct_owned_job) return false;
        BlockedAheadState::Stage owned;
        owned.program = std::move(*program);
        owned.flat.assign(pgm->user_data.begin(), pgm->user_data.end());
        state.bytes += owned.program.code.capacity() * sizeof(u32) + owned.flat.capacity() * sizeof(u32);
        if (state.bytes > MaxStateBytes) return false;
        state.stages[stage] = std::move(owned);
        ++active;
    }
    return active != 0;
#endif
}

void PipelineCache::BuildQueuedStates(const DrawIndirectParams params, bool restore) {
    auto* states = liverpool->PipeReadAheadStates();
    if (!states || (states->Empty() && blocked_ahead_states.empty())) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    if (!queued_state_regs) {
        // A queued state holds every register a key is built from; the others keep these values.
        queued_state_regs = std::make_unique<AmdGpu::Regs>(liverpool->DrawRegs());
    }
    Common::PhaseTimer eval_timer{Common::Phase::ReadAheadEval};
    const auto key = S().graphics_key;
    const bool key_was_legacy = S().key_uses_legacy; // D10: made again the same way below
    const auto* previous_regs = regs_override;
    u32 evaluated = 0;
    u32 started = 0;
    // Check time and the required build between states. One key evaluation cannot be preempted.
    static thread_local ReadAheadBudget read_ahead_budget;
    const bool waiting = restore || Common::CompileCoresWide();
    const auto budget = read_ahead_budget.Deadline(start, waiting);
    if (!budget) return;
    SCOPE_EXIT {
        if (!waiting) read_ahead_budget.Charge(start, std::chrono::steady_clock::now(), false);
    };
    std::exception_ptr restore_error;
    {
    SCOPE_EXIT {
        regs_override = previous_regs;
        try {
            S().draw_indirect_params = params;
            if (restore && evaluated > 0) {
                const bool was_forced = force_legacy;
                SCOPE_EXIT { force_legacy = was_forced; };
                force_legacy = key_was_legacy && legacy_fallback;
                RefreshGraphicsKey();
                ASSERT_MSG(S().graphics_key == key, "Read-ahead changed the current pipeline key");
            }
        } catch (...) {
            restore_error = std::current_exception();
        }
    };
    const auto in_budget = [&] {
        return std::chrono::steady_clock::now() < *budget && (!restore || evaluated < 64) &&
               (!t_required_pipeline_build ||
                t_required_pipeline_build->wait_for(std::chrono::microseconds{0}) !=
                    std::future_status::ready);
    };
    const auto evaluate = [&] {
        ++evaluated;
        S().draw_indirect_params = {};
        read_ahead_blocked = false;
        const bool valid = TrySpeculativeKey(regs_override, queued_state_regs.get(),
                                             [&] { return RefreshGraphicsKey(); });
        if (!valid || !HasSupportedColorTargets() || graphics_pipelines.contains(S().graphics_key)) {
            return !read_ahead_blocked;
        }
        if (const auto pending = pending_builds.find(S().graphics_key);
            pending != pending_builds.end()) {
            // PERF-038: a stored pipeline still waiting in the background queue is needed soon;
            // build it now from this draw's state instead.
            if (pending->second->preloading && CancelBuild(*pending->second)) {
                pending.value() = StartPipelineBuild(false);
#ifndef NDEBUG
                ++prewarm_stats.promoted;
#endif
                ++started;
            }
            return true;
        }
        pending_builds.emplace(S().graphics_key, StartPipelineBuild(false));
        ++started;
        return true;
    };
    if (RetryBlockedReadAhead() && !force_legacy && !fallback_nested) {
        const auto now = std::chrono::steady_clock::now();
        for (size_t remaining = blocked_ahead_states.size(); remaining && in_budget(); --remaining) {
            auto item = blocked_ahead_states.TakeReady(now);
            if (!item) break;
            auto blocked = std::move(*item);
            if (!RestoreRetryRegisters(queued_state_regs->reg_array, blocked.ranges)) continue;
            const auto* previous_inputs = std::exchange(retry_ahead_inputs, &blocked);
            SCOPE_EXIT { retry_ahead_inputs = previous_inputs; };
            if (!evaluate()) {
                blocked_ahead_states.Retry(std::move(blocked), now);
            }
        }
    } else if (!RetryBlockedReadAhead()) {
        blocked_ahead_states.clear();
    }
    while (in_budget() && states->Take(queued_state_regs->reg_array)) {
        if (!evaluate() && RetryBlockedReadAhead()) {
            // FIX-GT7-005: hull constants are mutable and cannot safely be retried later.
            using Stages = AmdGpu::ShaderStageEnable::VgtStages;
            const auto stages = queued_state_regs->stage_enable.raw;
            if (stages == Stages::LsHs || stages == Stages::LsHsEsGs) {
#ifndef NDEBUG
                ++queued_state_stats.retry_unproven;
#endif
                continue;
            }
            try {
                BlockedAheadState blocked;
                blocked.first = std::chrono::steady_clock::now();
                blocked.next = blocked.first + std::chrono::milliseconds{5};
                blocked.ranges = CaptureRetryRegisters(queued_state_regs->reg_array);
                if (blocked.ranges.empty()) continue;
                [[maybe_unused]] const bool retained = CaptureBlockedAheadInputs(*queued_state_regs, blocked) &&
                                      blocked_ahead_states.Keep(std::move(blocked));
#ifndef NDEBUG
                ++(retained ? queued_state_stats.retry_retained : queued_state_stats.retry_unproven);
#endif
            } catch (const std::bad_alloc&) {
                // Read-ahead is optional; a required draw still evaluates normally.
            }
        }
    }
    if (started > 0 && build_workers) {
        // PERF-038: the game needs pipelines; background builds wait.
        build_workers->PauseBackground(std::chrono::steady_clock::now() + std::chrono::seconds{2});
    }
    } // Scope guard restores the current draw state before any return.
    if (restore_error) std::rethrow_exception(restore_error);
#ifndef NDEBUG
    auto& stats = queued_state_stats;
    const auto now = std::chrono::steady_clock::now();
    stats.evaluated += evaluated;
    stats.started += started;
    stats.ms += std::chrono::duration<double, std::milli>(now - start).count();
    if (now - stats.since >= std::chrono::seconds{2}) {
        LOG_WARNING(Render_Vulkan,
                    "PERF-034 pipeline read-ahead in {:.1f} s: {} draw states from the command "
                    "thread evaluated, {} builds started, {:.1f} ms; {} draws waited for a build "
                    "({:.1f} ms); {} pending; owned retries retained {}, unproven {}",
                    std::chrono::duration<double>(now - stats.since).count(), stats.evaluated,
                    stats.started, stats.ms, stats.waits, stats.wait_ms, pending_builds.size(),
                    stats.retry_retained, stats.retry_unproven);
        stats = {};
        stats.since = now;
    }
#endif
}

void PipelineCache::WaitForBuild(PipelineBuild& build, const DrawIndirectParams params) {
    using namespace std::chrono_literals;
    const auto* inherited_required = t_required_pipeline_build;
    t_required_pipeline_build = nullptr;
    SCOPE_EXIT { t_required_pipeline_build = inherited_required; };
    // Limit this context to required-wait evaluation; batching retains its own policy.
    const auto evaluate_required = [&] {
        const auto* previous = t_required_pipeline_build;
        t_required_pipeline_build = &build.done;
        SCOPE_EXIT { t_required_pipeline_build = previous; };
        BuildQueuedStates(params, true);
    };
    // Do not evaluate unrelated queued states before checking a completed build.
    if (build.done.wait_for(std::chrono::microseconds{0}) != std::future_status::ready) {
        evaluate_required();
    }
    // An urgent build goes to the front of the compiler queue. While it is pending this
    // thread can discover more builds, but it never claims a compiler job itself.
    if (build.urgent) {
        const auto grace = std::chrono::steady_clock::now() + 200us;
        while (!build.started.load() && std::chrono::steady_clock::now() < grace) {
            std::this_thread::yield();
        }
    }
    while (build.done.wait_for(500us) != std::future_status::ready) {
        evaluate_required();
    }
#ifdef RYTY_GPU
    // PERF-101: GPU.compile_batch_ms (launcher "Shader compile batching"): with the game paused
    // for a compile anyway, the builds of the upcoming draws the command thread read ahead are
    // finished in the same pause, up to that long, so later draws pause less often.
    static const u32 batch_ms = static_cast<u32>(std::max(0, EmulatorSettings.GetCompileBatchMs()));
    if (batch_ms != 0 && !AsyncPipelines() && build_workers) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{batch_ms};
        BuildQueuedStates(params, true);
        while (!build_workers->ForegroundIdle() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(500us);
            BuildQueuedStates(params, true);
        }
    }
#endif
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipelineUnlocked(
    const DrawIndirectParams params, std::unique_lock<LookupLock>& lock) {
    ASSERT(lock.owns_lock());
    static const bool enabled = [] {
        const char* value = std::getenv("RYGT_ONE_OFF_UNLOCKED");
        return !value || value[0] != '0';
    }();
    if (!enabled) return GetGraphicsPipeline(params);
    auto* previous = std::exchange(t_required_lookup_lock, &lock);
    SCOPE_EXIT {
        t_required_lookup_lock = previous;
        unlocked_translations.clear();
        unlocked_listings.clear();
    };
    for (u32 attempt = 0; attempt < 8; ++attempt) {
        unlocked_translations.clear();
        unlocked_listings.clear();
        const auto* pipeline = GetGraphicsPipeline(params);
        if (pipeline || (unlocked_translations.empty() && unlocked_listings.empty())) return pipeline;
        // Speculative and legacy modes use shared lookup state and may not cross an unlock.
        if (!CanUnlockRequired(regs_override != nullptr, force_legacy, fallback_nested,
                               defer_misses)) {
            t_required_lookup_lock = nullptr;
            return GetGraphicsPipeline(params);
        }
        auto jobs = std::move(unlocked_translations);
        auto listings = std::move(unlocked_listings);
        t_required_lookup_lock = nullptr;
        WaitOutsideLookup(lock, unlocked_required_waits, [&] {
            Common::GuestClock::CompilePause pause;
            for (const auto& job : jobs) {
                std::unique_lock job_lock{job->mutex};
                job->cv.wait(job_lock, [&] { return job->done.load(); });
            }
            for (const auto& listing : listings) listing.wait();
        });
        t_required_lookup_lock = &lock;
    }
    // Bounded retries: preserve the preexisting required-draw fallback.
    t_required_lookup_lock = nullptr;
    return GetGraphicsPipeline(params);
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params) {
    UpdateCatchUp(std::chrono::steady_clock::now());
    // PERF-034: builds for the upcoming draws the command thread read ahead start first.
    BuildQueuedStates(params, false);
    // PERF-038: then, while no draw waits for pipelines, stored ones go to the background.
    InstallPrewarmed();
    S().draw_indirect_params = params;
    ++draws_since_scan;
    // FIX-078: missing translations of this draw's stages are queued together first; then the
    // draws after it are read and their shaders queued too, and the key is made again, waiting
    // for the workers' results (the slowest one, not their sum).
    defer_misses = AsyncTranslation();
    deferred_miss = false;
    fallback_miss = false;
    translation_skipped = false; // SHD-005
    bool key_valid = RefreshGraphicsKey();
    // D10: a new translation this draw needs is being made; the old renderer's draw it.
    if (!key_valid && fallback_miss) {
        fallback_nested = true;
        force_legacy = true;
        const bool legacy_valid = RefreshGraphicsKey() && !S().legacy_failed;
        force_legacy = false;
        const GraphicsPipeline* legacy = legacy_valid ? PipelineForCurrentKey(params) : nullptr;
#ifndef NDEBUG
        ++(legacy ? legacy_stats.fallback_ok : legacy_stats.fallback_none);
#endif
        ReportLegacy();
        if (legacy) {
            fallback_nested = false;
            return legacy;
        }
        // No old translations: the usual way (the queued translations are taken when ready).
        deferred_miss = false;
        key_valid = RefreshGraphicsKey();
    }
    struct NestedReset {
        bool& flag;
        ~NestedReset() {
            flag = false;
        }
    } nested_reset{fallback_nested};
    // One stage is queued per pass (a stage's start bindings follow from the stages before it);
    // the read-ahead runs once, after the first.
    for (u32 pass = 0; !key_valid && deferred_miss && pass < 6; ++pass) {
        if (pass == 0) {
            liverpool->ReadAheadNow();
            BuildQueuedStates(params, false);
            S().draw_indirect_params = params;
        }
        deferred_miss = false;
        key_valid = RefreshGraphicsKey();
        if (!key_valid && t_required_lookup_lock &&
            (!unlocked_translations.empty() || !unlocked_listings.empty())) break;
        if (!key_valid && translation_skipped) {
            break; // SHD-005
        }
    }
    defer_misses = false;
    if (!key_valid && t_required_lookup_lock &&
        (!unlocked_translations.empty() || !unlocked_listings.empty())) {
        deferred_miss = false;
        return nullptr;
    }
    if (!key_valid && translation_skipped) {
        // SHD-005: skipped while its shaders translate; a later draw takes them.
        deferred_miss = false;
        translation_skipped = false;
#ifndef NDEBUG
        ++async_stats.skipped_translating;
#endif
        NoteSkippedDraw();
        return nullptr;
    }
    if (!key_valid && deferred_miss) {
        deferred_miss = false;
        key_valid = RefreshGraphicsKey();
    }
    if (!key_valid) {
        return nullptr;
    }
    if (regs_override == nullptr) {
        const auto now = std::chrono::steady_clock::now();
        if (now - stale_ahead_checked >= std::chrono::seconds{2}) {
            stale_ahead_checked = now;
            ExpireStaleAheadBuilds(pending_builds, S().graphics_key, now);
        }
    }
    // D10: a key of new translations whose pipeline is not built yet. It is built in the
    // background; meanwhile the draw uses the key made of the old renderer's translations.
    if (legacy_fallback && !MenusWait() && !fallback_nested && !S().key_uses_legacy && regs_override == nullptr &&
        !graphics_pipelines.contains(S().graphics_key)) {
        const auto pending = pending_builds.find(S().graphics_key);
        const bool ready = pending != pending_builds.end() &&
                           pending->second->done.wait_for(std::chrono::seconds{0}) ==
                               std::future_status::ready;
        if (!ready && S().graphics_key.num_color_attachments <= AmdGpu::NUM_COLOR_BUFFERS) {
            if (pending == pending_builds.end()) {
                auto build = StartPipelineBuild(false);
                build->async = true;
                pending_builds[S().graphics_key] = build;
            }
            fallback_nested = true;
            force_legacy = true;
            const bool legacy_valid = RefreshGraphicsKey() && !S().legacy_failed;
            force_legacy = false;
            const GraphicsPipeline* legacy =
                legacy_valid ? PipelineForCurrentKey(params) : nullptr;
            fallback_nested = false;
#ifndef NDEBUG
            ++(legacy ? legacy_stats.fallback_ok
                      : legacy_valid ? legacy_stats.fallback_unbuilt : legacy_stats.fallback_none);
#endif
            ReportLegacy();
            if (legacy) {
                return legacy;
            }
            // No old translations for every stage: the new key again, the usual way.
            if (!RefreshGraphicsKey()) {
                return nullptr;
            }
        }
    }
    return PipelineForCurrentKey(params);
}

const GraphicsPipeline* PipelineCache::LookupBuiltPipeline(const DrawIndirectParams params) {
    // NAT-031: the key is computed in this thread's own scratch; the cache's own (the threads
    // above's last selection) is not touched.
    static thread_local KeyScratch scratch;
    KeyScratch* const saved = std::exchange(t_scratch, &scratch);
    t_lookup_only = true;
    scratch.draw_indirect_params = params;
    scratch.lookup_miss = nullptr;
    const GraphicsPipeline* found = nullptr;
    if (RefreshGraphicsKey()) {
        if (const auto it = graphics_pipelines.find(scratch.graphics_key);
            it != graphics_pipelines.end()) {
            found = it->second.get();
            found->NoteRetentionUse(std::chrono::steady_clock::now());
        }
        if (!found) {
            scratch.lookup_miss = "pipeline not built";
        }
    } else if (!scratch.lookup_miss) {
        scratch.lookup_miss = "no key (unsupported state or hull constants)";
    }
    t_lookup_only = false;
    t_scratch = saved;
    t_job_infos = &scratch.infos;
    // The miss reason is read through LookupMiss() on this thread right after: keep it.
    t_last_miss = scratch.lookup_miss;
    return found;
}

Shader::Info& PipelineCache::MatchInfo(const Shader::Info& cached) {
    struct Copy {
        u64 identity{};
        std::unique_ptr<Shader::Info> info;
    };
    static thread_local std::unordered_map<const Shader::Info*, Copy> copies;
    if (copies.size() > 8192) {
        copies.clear(); // programs come and go: don't keep copies of retired ones forever
    }
    auto& copy = copies[&cached];
    if (!copy.info || copy.identity != cached.identity) {
        copy.info = std::make_unique<Shader::Info>(cached);
        copy.identity = cached.identity;
    }
    return *copy.info;
}

const GraphicsPipeline* PipelineCache::PipelineForCurrentKey(const DrawIndirectParams params) {
    if (S().graphics_key.num_color_attachments > AmdGpu::NUM_COLOR_BUFFERS) {
        LOG_WARNING(Render_Vulkan, "Skipping draw with {} color attachments (MRT mask={:#x})",
                    S().graphics_key.num_color_attachments, S().graphics_key.mrt_mask);
        return nullptr;
    }
    for (u32 cb = 0; cb < S().graphics_key.num_color_attachments; ++cb) {
        const auto& color_buffer = S().graphics_key.color_buffers[cb];
        if (color_buffer.data_format != AmdGpu::DataFormat::FormatInvalid &&
            Vulkan::LiverpoolToVK::TrySurfaceFormat(
                color_buffer.data_format, color_buffer.num_format) == vk::Format::eUndefined) {
            LOG_WARNING(Render_Vulkan,
                        "Skipping draw with unsupported color target {} format: data={}, number={}",
                        cb, static_cast<u32>(color_buffer.data_format),
                        static_cast<u32>(color_buffer.num_format));
            return nullptr;
        }
    }
    // PERF-GT7-017: avoid creating and erasing a placeholder for each draw while
    // its optional asynchronous pipeline is still building. Required draws and
    // completed builds continue through the normal lookup and installation path.
    if (CompileMode() == 2 && !graphics_pipelines.contains(S().graphics_key)) {
        const auto pending = pending_builds.find(S().graphics_key);
        if (pending != pending_builds.end() && pending->second->async &&
            pending->second->done.wait_for(std::chrono::microseconds{0}) !=
                std::future_status::ready &&
            !LegacyWaits() && !WritesMemory(S().infos) && !NeverSkipped(S().infos) &&
            !MenusWait()) {
#ifndef NDEBUG
            ++async_stats.skipped_draws;
#endif
            NoteSkippedDraw();
            NoteAsyncSkip(pending->second, true);
            return nullptr;
        }
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(S().graphics_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(S().graphics_key);
        // An async draw retries an empty map slot until its existing build completes.
        // Do not repeat new-key diagnostics (or megabytes of logs) for those polls.
        if (!pending_builds.contains(S().graphics_key)) {
            LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
#ifndef NDEBUG
            // DIAG-050: the parts of new keys of tessellated pipelines (GT Sport's race summary built
            // hundreds of keys for the same few programs).
            // DIAG-051: which part of a new key differs from the last key with the same stages
            // (GT Sport's race summary asks for thousands of new pipelines).
            {
                static std::unordered_map<u64, GraphicsPipelineKey> last_by_stages;
                static std::unordered_map<std::string, u32> diff_counts;
                static u32 keys_seen = 0;
                const u64 stages_hash = XXH3_64bits(S().graphics_key.stage_hashes.data(),
                                                    sizeof(S().graphics_key.stage_hashes));
                auto [prev, first] = last_by_stages.try_emplace(stages_hash, S().graphics_key);
                std::string fields = first ? "new stages" : "";
                if (!first) {
                    const auto& a = prev->second;
                    const auto& b = S().graphics_key;
                    const auto differs = [&](const auto& x, const auto& y) {
                        return std::memcmp(&x, &y, sizeof(x)) != 0;
                    };
                    const auto add = [&](const char* name) {
                        fields += fields.empty() ? name : std::string(",") + name;
                    };
                    if (differs(a.vertex_buffer_formats, b.vertex_buffer_formats)) add("vtx");
                    if (a.patch_control_points != b.patch_control_points) add("pcp");
                    if (a.num_color_attachments != b.num_color_attachments) add("ncb");
                    if (differs(a.color_buffers, b.color_buffers)) add("cb");
                    if (differs(a.blend_controls, b.blend_controls)) add("blend");
                    if (differs(a.write_masks, b.write_masks)) add("mask");
                    if (differs(a.cb_shader_mask, b.cb_shader_mask)) add("shmask");
                    if (a.logic_op != b.logic_op) add("logic");
                    if (a.num_samples != b.num_samples || a.depth_samples != b.depth_samples ||
                        differs(a.color_samples, b.color_samples)) add("samples");
                    if (a.mrt_mask != b.mrt_mask) add("mrt");
                    if (a.z_format != b.z_format || a.stencil_format != b.stencil_format ||
                        a.depth_clamp_enable != b.depth_clamp_enable) add("depth");
                    if (a.prim_type != b.prim_type || a.polygon_mode != b.polygon_mode ||
                        a.clip_space != b.clip_space || a.provoking_vtx_last != b.provoking_vtx_last ||
                        a.depth_clip_enable != b.depth_clip_enable) add("raster");
                    if (fields.empty()) {
                        add("padding");
                        static u32 logged = 0;
                        if (logged++ < 10) {
                            const auto* x = reinterpret_cast<const u8*>(&a);
                            const auto* y = reinterpret_cast<const u8*>(&b);
                            std::string offsets;
                            for (size_t i = 0; i < sizeof(GraphicsPipelineKey); ++i) {
                                if (x[i] != y[i]) {
                                    offsets += fmt::format(" {}:{:02x}/{:02x}", i, x[i], y[i]);
                                }
                            }
                            LOG_WARNING(Render_Vulkan,
                                        "DIAG-051: keys differ only outside named fields (size {}, "
                                        "color_buffers at {}, mrt_mask at {}):{}",
                                        sizeof(GraphicsPipelineKey),
                                        offsetof(GraphicsPipelineKey, color_buffers),
                                        offsetof(GraphicsPipelineKey, mrt_mask), offsets);
                        }
                    }
                    prev->second = S().graphics_key;
                }
                ++diff_counts[fields];
                if (++keys_seen % 250 == 0) {
                    std::string summary;
                    for (const auto& [name, count] : diff_counts) {
                        summary += fmt::format(" {}={}", name, count);
                    }
                    LOG_WARNING(Render_Vulkan, "DIAG-051: {} new keys; differences from the last "
                                "key with the same stages:{}", keys_seen, summary);
                }
            }
            // SHADGT_LOG_KEYS=1 logs every new key the same way (up to 5000).
            static const bool log_all_keys = [] {
                const char* env = std::getenv("SHADGT_LOG_KEYS");
                return env && env[0] == '1';
            }();
            if (log_all_keys ||
                S().graphics_key.stage_hashes[u32(Shader::SwStage::TessellationControl)] != 0) {
                static std::atomic<u32> logged{};
                if (const u32 n = ++logged; n <= (log_all_keys ? 5000u : 80u)) {
                    const auto& k = S().graphics_key;
                    const auto part = [](const auto& field) {
                        return XXH3_64bits(&field, sizeof(field)) & 0xffff;
                    };
                    LOG_WARNING(Render_Vulkan,
                                "DIAG-050: new key {} {:#x}: stages {:#x} {:#x} {:#x} {:#x} {:#x}; "
                                "pcp {} prim {} vtx {:#x} cb {:#x} blend {:#x} mask {:#x} "
                                "shmask {:#x} samples {} {} mrt {:#x} z {} clip {}; all {:#x}",
                                n, pipeline_hash, k.stage_hashes[0], k.stage_hashes[1],
                                k.stage_hashes[2], k.stage_hashes[3], k.stage_hashes[4],
                                k.patch_control_points,
                                u32(k.prim_type), part(k.vertex_buffer_formats), part(k.color_buffers),
                                part(k.blend_controls), part(k.write_masks), part(k.cb_shader_mask),
                                k.num_samples, k.depth_samples, k.mrt_mask, u32(k.z_format),
                                u32(k.depth_clip_enable),
                                XXH3_64bits(&k, sizeof(k)) & 0xffffff);
                }
            }

#endif
        }

        GraphicsPipeline::SerializationSupport sdata{};
        // PERF-019: the pipeline may already be building from an earlier read-ahead. If not,
        // it starts building now, and the draws after it are read for more new pipelines to
        // build at the same time on other cores.
        std::shared_ptr<PipelineBuild> build;
        bool predicted = false;
        auto pending = pending_builds.find(S().graphics_key);
        if (pending != pending_builds.end() &&
            (pending->second->preloading || pending->second->async)) {
            auto stored = pending->second;
            if (stored->done.wait_for(stored->async ? QuickWait() : std::chrono::microseconds{0}) ==
                std::future_status::ready) {
                stored->done.get();
                // PERF-038: built in the background before the draw needed it; no miss.
                pending_builds.erase(pending);
                stored->pipeline->SetStageInfos(S().infos);
                sdata = stored->sdata;
                it.value() = std::move(stored->pipeline);
#ifndef NDEBUG
                ++(stored->async ? async_stats.installed : prewarm_stats.hits);
#endif
                // Stored again, so the next session builds it among the first.
                if (!S().key_uses_legacy) {
                    RegisterPipelineData(S().graphics_key, pipeline_hash, sdata);
                }
                retention_now = std::chrono::steady_clock::now();
                pipeline_last_use[it->second.get()] = retention_now;
                return it->second.get();
            }
            if ((stored->async || (CompileMode() == 2 && AsyncPipelines())) &&
                !LegacyWaits() && !WritesMemory(S().infos) && !NeverSkipped(S().infos) &&
                !MenusWait()) {
                // PERF-076: still building; this draw is skipped, the next ones look again.
                graphics_pipelines.erase(it);
#ifndef NDEBUG
                ++async_stats.skipped_draws;
#endif
                NoteSkippedDraw(); // PERF-103
                NoteAsyncSkip(stored, true);
                return nullptr;
            }
            if (!(t_required_lookup_lock &&
                  CanUnlockRequired(regs_override != nullptr, force_legacy, fallback_nested,
                                    defer_misses) && PreservesDepthOutput()) &&
                CancelBuild(*stored)) {
                // Not started in the background yet: built now the usual way.
                pending_builds.erase(pending);
                pending = pending_builds.end();
#ifndef NDEBUG
                ++prewarm_stats.cancelled;
#endif
            }
        }
        if (build_workers && CompileMode() != 2) {
            build_workers->PauseBackground(std::chrono::steady_clock::now() +
                                           std::chrono::seconds{2});
        }
        ++Common::GetWorkCounters().pipelines_compiled;
        last_pipeline_miss = std::chrono::steady_clock::now();
        GraphicsPipeline::NotePipelineMiss();
        // PERF-109 (WillScarlettOhara, PERF-GT7-025): builds for draws that go on
        // run in draw order, after waiting draws and before read-ahead.
        const bool goes_on = AsyncPipelines() && !LegacyWaits() && !WritesMemory(S().infos) &&
                             !NeverSkipped(S().infos) && !MenusWait();
        auto* const queued_states = liverpool->PipeReadAheadStates();
        if (queued_states) {
            queued_states->NoteMiss();
        }
        if (pending != pending_builds.end()) {
            build = pending->second;
            pending_builds.erase(pending);
            predicted = true;
            // PERF-031: the read-ahead reads the command thread's state; not with the draw pipe.
            if (Common::PerfFeatureEnabled(19) && !liverpool->Pipelined() &&
                draws_since_scan >= last_scan_draws) {
                // Past the last read-ahead: keep reading from where it stopped.
                const auto key = S().graphics_key;
                ReadAhead(params);
                ASSERT_MSG(S().graphics_key == key, "Read-ahead changed the current pipeline key");
            }
        } else if (Common::PerfFeatureEnabled(19) && !liverpool->Pipelined()) {
            const auto key = S().graphics_key;
            build = StartPipelineBuild(true, goes_on);
            ReadAhead(params);
            ASSERT_MSG(S().graphics_key == key, "Read-ahead changed the current pipeline key");
        } else if (queued_states) {
            // PERF-034: a worker builds it while this thread starts the builds the command
            // thread's read-ahead queues meanwhile.
            build = StartPipelineBuild(true, goes_on);
        } else {
            // Queue even when read-ahead/draw-pipe helpers are disabled. Retain the job
            // in pending_builds below so a later draw installs its completed pipeline.
            build = StartPipelineBuild(true, goes_on);
        }
        // PERF-076: a draw does not wait for a new pipeline. Its build goes on on a worker, the
        // draws that need it are skipped until it is done (a frame or two of a missing object),
        // and the game does not stall for the 60-140 ms a build takes. SHADGT_ASYNC_PIPELINES=0
        // or -DisablePerf 76 waits as before.
        // PERF-103: in slow-motion mode every new pipeline goes on in the background at once
        // and the game goes on in slow motion until it is built.
        // D10: a pipeline of old translations is built right away (the driver usually has it
        // from the old renderer's sessions, so it takes milliseconds).
        if (build && AsyncPipelines() && !LegacyWaits() && !WritesMemory(S().infos) &&
            !NeverSkipped(S().infos) && !MenusWait() &&
            build->done.wait_for(QuickWait()) != std::future_status::ready) {
            build->async = true;
            pending_builds[S().graphics_key] = build;
            graphics_pipelines.erase(it);
#ifndef NDEBUG
            ++async_stats.deferred;
#endif
#ifndef NDEBUG
            ++async_stats.skipped_draws;
#endif
            NoteSkippedDraw(); // PERF-103
            NoteAsyncSkip(build, false);
            return nullptr;
        }
        if (build) {
            // A read-ahead job becomes urgent once this draw actually needs it.
            // Requeue the shared build at the front rather than waiting behind
            // unrelated cosmetic jobs. FinishBuild's atomic claim permits only
            // one worker to compile it; the old queued closure becomes a no-op.
            if (!build->urgent && !build->started.load(std::memory_order_acquire) &&
                build->done.wait_for(std::chrono::microseconds{0}) != std::future_status::ready) {
                build->urgent = true;
                QueueBuild(build, BuildQueue::Kind::Urgent);
            }
            if (t_required_lookup_lock &&
                CanUnlockRequired(regs_override != nullptr, force_legacy, fallback_nested,
                                  defer_misses) && PreservesDepthOutput() &&
                build->done.wait_for(std::chrono::microseconds{0}) != std::future_status::ready) {
                // Keep the job discoverable while the recorder sleeps. Do not retain an
                // iterator or draw-stage pointers across the unlock: another lookup can
                // install the result or rehash the maps. Recompute the key on return.
                pending_builds[S().graphics_key] = build;
                graphics_pipelines.erase(it);
                auto* lock = std::exchange(t_required_lookup_lock, nullptr);
                ++build->required_waiters;
                SCOPE_EXIT { --build->required_waiters; };
                WaitOutsideLookup(*lock, unlocked_required_waits, [&] {
                    Common::GuestClock::CompilePause pause;
                    Common::SetCompileCoresWide(true);
                    SCOPE_EXIT { Common::SetCompileCoresWide(false); };
                    while (build->done.wait_for(std::chrono::milliseconds{5}) !=
                           std::future_status::ready) {
                        if (RetryBlockedReadAhead() && lock->try_lock()) {
                            SCOPE_EXIT { lock->unlock(); };
                            const auto* previous_required = t_required_pipeline_build;
                            t_required_pipeline_build = &build->done;
                            SCOPE_EXIT { t_required_pipeline_build = previous_required; };
                            BuildQueuedStates(params, false);
                        }
                    }
                });
                return GetGraphicsPipeline(params);
            }
#ifndef NDEBUG
            const auto wait_start = std::chrono::steady_clock::now();
            if (GeometryTrace::current) GeometryTrace::Record("wait_begin", GeometryTrace::current, pipeline_hash);
#endif
            // Queueing a cosmetic job must not freeze input/audio clocks. Hold
            // them only while this command actually needs an unfinished build.
            std::optional<Common::GuestClock::CompilePause> hold;
            const bool widen_compile = build->done.wait_for(std::chrono::microseconds{0}) !=
                                       std::future_status::ready;
            if (widen_compile) {
                hold.emplace();
                Common::SetCompileCoresWide(true);
            }
            SCOPE_EXIT { if (widen_compile) Common::SetCompileCoresWide(false); };
            if (queued_states) {
                Common::Nvtx::Scope nvtx{"wait for pipeline build"}; // DIAG-056
                WaitForBuild(*build, params);
            }
            // Only compiler workers claim and execute builds. Required draws can wait for
            // completion, but never run the compiler on the GPU command/recorder thread.
            build->done.get();
            hold.reset();
#ifndef NDEBUG
            const auto now = std::chrono::steady_clock::now();
            if (GeometryTrace::current) {
                GeometryTrace::Record("wait_end", GeometryTrace::current, pipeline_hash);
                if (now - wait_start >= std::chrono::milliseconds{50}) GeometryTrace::Trigger();
            }
            if (queued_states) {
                ++queued_state_stats.waits;
                queued_state_stats.wait_ms +=
                    std::chrono::duration<double, std::milli>(now - wait_start).count();
            }
            ++Common::GetWorkCounters().pipeline_waits;
            Common::GetWorkCounters().pipeline_wait_us += u64(
                std::chrono::duration_cast<std::chrono::microseconds>(now - wait_start).count());
            RyDiag::RecordElapsed(RyDiag::Zone::PipelineWait, now - wait_start, pipeline_hash);
            // DIAG-124: why this draw waited instead of being skipped.
            std::string why;
            if (!AsyncPipelines()) why += " no-async";
            if (S().key_uses_legacy) why += " legacy";
            if (WritesMemory(S().infos)) why += " writes-memory";
            if (NeverSkipped(S().infos)) why += " required-output";
            if (MenusWait()) why += " menus";
            LOG_WARNING(Render_Vulkan,
                        "Pipeline {:#x} {}: built {:.1f} ms after it was queued, draw waited "
                        "{:.1f} ms (DIAG-124:{})",
                        pipeline_hash, predicted ? "read ahead" : "on demand",
                        std::chrono::duration<double, std::milli>(now - build->queued).count(),
                        std::chrono::duration<double, std::milli>(now - wait_start).count(),
                        why.empty() ? " ready within the quick wait" : why);
#endif
            build->pipeline->SetStageInfos(S().infos);
            sdata = build->sdata;
            it.value() = std::move(build->pipeline);
        }

        if (!S().key_uses_legacy) { // D10: old translations are not in this store
            RegisterPipelineData(S().graphics_key, pipeline_hash, sdata);
        } else {
#ifndef NDEBUG
            ++legacy_stats.pipelines;
#endif
        }
        ++num_new_pipelines;
        MaybeSaveDriverCache();

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (S().infos[stage]) {
                    auto& m = S().modules[stage];
                    module_related_pipelines[m].emplace_back(S().graphics_key);
                }
            }
        }
    }
    if (is_new) retention_now = std::chrono::steady_clock::now();
    pipeline_last_use[it->second.get()] = retention_now;
    return it->second.get();
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    fallback_miss = false;
    bool key_valid = RefreshComputeKey();
    if (!key_valid && fallback_miss) {
        // D10: as for graphics, the old translation while the new one is made.
        fallback_nested = true;
        force_legacy = true;
        key_valid = RefreshComputeKey() && !S().legacy_failed;
        force_legacy = false;
#ifndef NDEBUG
        ++(key_valid ? legacy_stats.fallback_ok : legacy_stats.fallback_none);
#endif
        if (!key_valid) {
            key_valid = RefreshComputeKey();
        }
        fallback_nested = false;
    }
    if (!key_valid) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        Common::GuestClock::CompilePause hold; // FIX-044
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);

        ComputePipeline::SerializationSupport sdata{};
        ++Common::GetWorkCounters().pipelines_compiled;
        RYDIAG_SCOPE(PipelineBuild, pipeline_hash);
        // Compute dispatches cannot be dropped: later draws may consume their writes. Own the
        // shader inputs on a bounded compiler worker and publish the completed pipeline before
        // recording the dispatch; do not hand the worker mutable command-thread state.
        struct ComputeBuild {
            Shader::Info info;
            std::vector<u32> user_data;
            ComputePipeline::SerializationSupport sdata{};
        };
        auto inputs = std::make_shared<ComputeBuild>();
        inputs->info = *S().infos[0];
        inputs->user_data.assign(inputs->info.user_data.begin(), inputs->info.user_data.end());
        inputs->info.user_data = inputs->user_data;
        const auto key = compute_key;
        const auto module = S().modules[0];
        auto job = std::make_shared<std::packaged_task<std::unique_ptr<ComputePipeline>()>>(
            [this, inputs, key, module] {
                Common::BeginForegroundCompile();
                SCOPE_EXIT { Common::EndForegroundCompile(); };
                return std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                    *pipeline_cache, key, inputs->info, module, inputs->sdata);
            });
        auto completion = job->get_future();
        if (!build_workers) build_workers = std::make_unique<PipelineBuildWorkers>();
        build_workers->Push([job] { (*job)(); }, BuildQueue::Kind::Urgent);
        it.value() = completion.get();
        it.value()->SetStageInfo(*S().infos[0]);
        if (const Shader::Info* info = S().infos[0]; WarmComputeOn() && info) {
            const auto dir = WarmComputeDir();
            const auto file = dir / fmt::format("{:#018x}-{:#018x}.bin", pipeline_hash,
                                                  ComputePipeline::RecipePolicy(instance, profile));
            std::error_code ec;
            if (!std::filesystem::exists(file, ec)) {
                std::optional<size_t> perm;
                if (const auto prog = program_cache.find(info->pgm_hash); prog != program_cache.end()) {
                    const auto& modules = prog->second->modules;
                    for (size_t i = 0; i < modules.size(); ++i)
                        if (modules[i].module == S().modules[0]) { perm = i; break; }
                }
                std::vector<u32> spirv;
                if (perm) Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                    fmt::format("{:#018x}_{}", info->pgm_hash, *perm), spirv);
                std::filesystem::create_directories(dir, ec);
                if (IsCompleteSpirv(spirv)) it.value()->SaveRecipe(file, spirv);
            }
        }
        sdata = inputs->sdata;
        if (!S().key_uses_legacy) { // D10
            RegisterPipelineData(compute_key, sdata);
        }
        ++num_new_pipelines;
        MaybeSaveDriverCache();

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = S().modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    if (is_new) retention_now = std::chrono::steady_clock::now();
    pipeline_last_use[it->second.get()] = retention_now;
    return it->second.get();
}

void PipelineCache::RefreshSwizzledBlend(u32 cb, Shader::PsColorBuffer& color_buffer,
                                         const AmdGpu::BlendControl& bc) {
    const auto& regs = Regs();
    // Dual-source blending needs attachment 0 as the only written color target. Hardware writes
    // another MRT only when it is bound, unmasked, and has a shader export format.
    const auto writes_other_mrt = [&] {
        for (u32 other = 1; other < AmdGpu::NUM_COLOR_BUFFERS; ++other) {
            if (regs.color_buffers[other] && regs.color_target_mask.GetMask(other) != 0 &&
                regs.color_export_format.GetFormat(other) != AmdGpu::ShaderExportFormat::Zero) {
                return true;
            }
        }
        return false;
    };
    const char* rejection = nullptr;
    if (cb != 0) {
        rejection = "not attachment 0";
    } else if (!instance.IsDualSourceBlendSupported()) {
        rejection = "dual-source blending unsupported";
    } else if (writes_other_mrt()) {
        rejection = "other MRT exports";
    } else if ((regs.color_shader_mask.GetMask(cb) & AmdGpu::ColorBufferMask::ComponentA) == 0) {
        rejection = "alpha not exported";
    }
    if (!rejection && LiverpoolToVK::NeedsSwizzledAlphaBlend(color_buffer.swizzle, bc)) {
        color_buffer.blend_swizzled_alpha = 1;
        return;
    }

    using NumberFormat = AmdGpu::NumberFormat;
    const auto num_format = color_buffer.num_format;
    const bool blendable_format =
        num_format == NumberFormat::Unorm || num_format == NumberFormat::Snorm ||
        num_format == NumberFormat::Srgb || num_format == NumberFormat::Float;
    if (!rejection && color_buffer.num_conversion != AmdGpu::NumberConversion::None) {
        rejection = "number conversion";
    } else if (!rejection && !blendable_format) {
        rejection = "number format";
    }
    const auto general = LiverpoolToVK::GetSwizzledFactorBlend(color_buffer.swizzle, bc);
    if (!rejection && general) {
        color_buffer.blend_swizzled_factors = 1;
        color_buffer.swizzled_color_src = general->color_src;
        color_buffer.swizzled_color_dst = general->color_dst;
        color_buffer.swizzled_alpha_src = general->alpha_src;
        color_buffer.swizzled_alpha_dst = general->alpha_dst;
        return;
    }
    if (!rejection) {
        rejection = "unsupported equation";
    }

    // Report each remaining configuration once; it still blends with the wrong lane order.
    const auto& swizzle = color_buffer.swizzle;
    const u64 config =
        u64(std::bit_cast<u32>(bc)) | u64(cb) << 32 | u64(u32(num_format)) << 36 |
        u64(swizzle.r) << 40 | u64(swizzle.g) << 44 | u64(swizzle.b) << 48 | u64(swizzle.a) << 52 |
        u64(writes_other_mrt()) << 56 |
        u64((regs.color_shader_mask.GetMask(cb) & AmdGpu::ColorBufferMask::ComponentA) != 0) << 57;
    static constexpr size_t MaxLoggedSwizzledBlends = 32;
    if (t_lookup_only || // NAT-031: job threads leave the log to the threads above
        logged_swizzled_blends.size() >= MaxLoggedSwizzledBlends ||
        !logged_swizzled_blends.insert(config).second) {
        return;
    }
    const auto blend = LiverpoolToVK::EffectiveBlend(bc);
    LOG_WARNING(Render_Vulkan,
                "Unhandled swizzled blend ({}): cb={} swizzle={},{},{},{} format={}/{} "
                "color={}*src {} {}*dst alpha={}*src {} {}*dst separate={} export_formats={:#x} "
                "shader_mask={:#x} target_mask={:#x}",
                rejection, cb, u32(swizzle.r), u32(swizzle.g), u32(swizzle.b), u32(swizzle.a),
                u32(color_buffer.data_format), u32(num_format), u32(blend.color_src),
                u32(blend.color_func), u32(blend.color_dst), u32(blend.alpha_src),
                u32(blend.alpha_func), u32(blend.alpha_dst), u32(bc.separate_alpha_blend),
                regs.color_export_format.raw, regs.color_shader_mask.raw,
                regs.color_target_mask.raw);
}

bool PipelineCache::RefreshGraphicsKey() {
    std::memset(&S().graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = Regs();
    auto& key = S().graphics_key;

    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();

    key.z_format = regs.depth_buffer.DepthValid() ? regs.depth_buffer.z_info.format
                                                  : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    // FIX-220: DB_ALPHA_TO_MASK. GT Sport draws the track-edge overlays (grass and kerb paint over
    // the asphalt) with alpha to coverage; without it their noise-masked alpha showed as dots.
    key.alpha_to_coverage = regs.db_alpha_to_mask & 1;
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type =
        QuadListAsTriangles() ? AmdGpu::PrimitiveType::TriangleList : regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        const auto data_format = col_buf.GetDataFmt();
        const auto number_format = col_buf.GetNumberFmt();
        if (Vulkan::LiverpoolToVK::TrySurfaceFormat(data_format, number_format) ==
            vk::Format::eUndefined) {
            LOG_WARNING(Render_Vulkan,
                        "Skipping draw with unsupported color target {} format: data={}, number={}",
                        cb, static_cast<u32>(data_format), static_cast<u32>(number_format));
            return false;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = data_format;
        color_buffer.num_format = number_format;
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

        const auto& bc = regs.blend_control[cb];
        color_buffer.blend_self_scale =
            bc.enable && !col_buf.info.blend_bypass &&
            (bc.color_func == AmdGpu::BlendControl::BlendFunc::Min ||
             bc.color_func == AmdGpu::BlendControl::BlendFunc::Max) &&
            bc.color_src_factor == AmdGpu::BlendControl::BlendFactor::SrcColor &&
            bc.color_dst_factor == AmdGpu::BlendControl::BlendFactor::DstColor;
        if (!col_buf.info.blend_bypass &&
            LiverpoolToVK::IsLaneDependentSwizzledBlend(color_buffer.swizzle, bc)) {
            RefreshSwizzledBlend(cb, color_buffer, bc);
        }
#ifdef RYTY_GPU
        // HDR-UI: GT Sport's UI blends SDR colours into the HDR10 display buffer.
        // Opaque draws too (the menus' solid panels and buttons), except the scene's own copy
        // into the display buffer, which samples the HDR scene (a float image; see below).
        color_buffer.hdr_ui_layer = cb == 0 && Ryty::g_hdr_output.load(std::memory_order_relaxed) &&
                                    Ryty::g_ui_layer_ready.load(std::memory_order_relaxed) &&
                                    Ryty::IsDisplayAddress(col_buf.Address());
        color_buffer.hdr_ui_alpha1 =
            color_buffer.hdr_ui_layer && !(bc.enable && !col_buf.info.blend_bypass);
#endif
    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages()) {
        return false;
    }
#ifdef RYTY_GPU
    // HDR-UI: an opaque draw into the display buffer that samples a float image is the scene's
    // copy (already HDR): it stays in the display buffer.
    if (key.color_buffers[0].hdr_ui_alpha1) {
        bool samples_float = false;
        if (const auto* fs = S().infos[u32(Shader::SwStage::Fragment)]) {
            for (const auto& image : fs->images) {
                const auto tsharp = image.GetSharp(*fs);
                samples_float |= tsharp.GetNumberFmt() == AmdGpu::NumberFormat::Float;
            }
        }
        if (samples_float) {
            key.color_buffers[0].hdr_ui_layer = 0;
            key.color_buffers[0].hdr_ui_alpha1 = 0;
            if (!RefreshGraphicsStages()) {
                return false;
            }
        }
    }
#endif

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};
        if (key.color_buffers[cb].hdr_ui_layer) {
            key.write_masks[cb] |= vk::ColorComponentFlagBits::eA; // HDR-UI: coverage
        }

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

#ifdef RYTY_GPU
    if (RytyGhost::ShouldSmooth(regs)) {
        key.blend_controls[0] = RytyGhost::SmoothBlendControl();
    }
#endif

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages() {
    const auto& regs = Regs();
    auto& key = S().graphics_key;
    S().fetch_shader = nullptr;
    S().stage_deferred = false; // FIX-078
    S().key_uses_legacy = false; // D10
    S().legacy_failed = false;

    // FIX-027: a shader program register pointing outside guest memory (GT Sport's dealership had
    // a pixel shader at 0x100) crashed the emulator reading the program. Skip such a draw, like
    // the GPU would fault on it rather than take the system down. Checked again only when an
    // address changes. -DisablePerf 36 reads it anyway.
    static const bool check_programs = Common::PerfFeatureEnabled(36);
    if (check_programs) {
        static std::array<VAddr, 6> checked{};
        for (u32 stage = 0; stage < checked.size(); ++stage) {
            const auto* pgm = regs.ProgramForStage(stage);
            const VAddr address = pgm ? pgm->Address<VAddr>() : 0;
            if (retry_ahead_inputs && regs.stage_enable.IsStageEnabled(stage) && address) {
                const auto& owned = retry_ahead_inputs->stages[stage];
                if (!owned || owned->program.guest_base != address) return false;
                continue;
            }
            if (!regs.stage_enable.IsStageEnabled(stage) || address == 0 ||
                address == checked[stage]) {
                continue;
            }
            auto* memory = Core::Memory::Instance();
            if (!memory->IsValidMapping(address, 8) || !memory->IsMappedAddress(address)) {
                static std::atomic<u32> skipped{};
                if (const u32 n = ++skipped; n <= 20 || n % 500 == 0) {
                    LOG_WARNING(Render_Vulkan,
                                "FIX-027: draw {} skipped: hardware stage {} program at {:#x} is "
                                "not in guest memory ({})",
                                n, stage, address, regs_override ? "read-ahead" : "draw");
                }
                return false;
            }
            if (!t_lookup_only) { // NAT-031: job threads only read the list
                checked[stage] = address;
            }
        }
    }

    Shader::Backend::Bindings binding{};
    bool stage_failed = false;
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            S().infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            S().infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* owned = retry_ahead_inputs ? &retry_ahead_inputs->stages[stage_in_idx] : nullptr;
        if (owned && (!*owned || (*owned)->program.guest_base != pgm->Address<VAddr>())) {
            stage_failed = true;
            return true;
        }
        if (owned && !DirectOnlyRetryProgram((**owned).program.hash)) {
            stage_failed = true;
            return true;
        }
        const auto params = [&]() -> Shader::ShaderParams {
            if (owned) {
                const auto& saved = **owned;
                return MakeOwnedShaderParams(pgm->user_data, saved.program);
            }
            return AmdGpu::GetParams(*pgm);
        }();
        Shader::ReplayInputs replay{};
        const auto* previous_replay = Shader::t_replay;
        if (owned) {
            replay.flat = &(**owned).flat;
            Shader::t_replay = &replay;
        }
        SCOPE_EXIT { Shader::t_replay = previous_replay; };
        std::tie(S().infos[stage_out_idx], S().modules[stage_out_idx], key.stage_hashes[stage_out_idx]) =
            GetProgram(stage_in, stage_out, params, binding);
        if (!S().modules[stage_out_idx]) {
            stage_failed = true; // FIX-034: the shader could not be translated
        }
        return true;
    };

    S().infos.fill(nullptr);
    S().modules.fill(nullptr);

    bind_stage(HwStage::Fragment, SwStage::Fragment);

    const auto* fs_info = S().infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);
    if (key.num_color_attachments > AmdGpu::NUM_COLOR_BUFFERS) {
        LOG_WARNING(Render_Vulkan, "Skipping draw with invalid MRT mask {:#x}", key.mrt_mask);
        return false;
    }

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (const auto* hull = S().infos[u32(SwStage::TessellationControl)];
            !hull || !SaneTessConstants(*hull, liverpool, regs_override != nullptr)) {
            return false; // FIX-058: the local stage reads the hull's constants
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (const auto* hull = S().infos[u32(SwStage::TessellationControl)];
            !hull || !SaneTessConstants(*hull, liverpool, regs_override != nullptr)) {
            return false; // FIX-058: the local stage reads the hull's constants
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }
    if (stage_failed) {
        return false;
    }

    const auto* vs_info = S().infos[static_cast<u32>(SwStage::Vertex)];
    if (vs_info && S().fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : S().fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            const auto data_format = buffer.GetDataFmt();
            const auto number_format = buffer.GetNumberFmt();
            const auto format = Vulkan::LiverpoolToVK::TrySurfaceFormat(data_format, number_format);
            if (format == vk::Format::eUndefined) {
                LOG_WARNING(Render_Vulkan,
                            "Skipping draw with unsupported vertex attribute {} format: data={}, "
                            "number={}",
                            vertex_binding, static_cast<u32>(data_format),
                            static_cast<u32>(number_format));
                return false;
            }
            key.vertex_buffer_formats[vertex_binding++] = format;
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    S().key_uses_legacy = false; // D10
    S().legacy_failed = false;
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->DrawCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    std::tie(S().infos[0], S().modules[0], compute_key.value) =
        GetProgram(HwStage::Compute, SwStage::Compute, cs_params, binding);
    return S().modules[0] != nullptr; // FIX-034: false when the shader could not be translated
}

} // namespace Vulkan
namespace Shader::Optimization {
extern thread_local std::span<const u32> g_diag_compiling_code;
extern thread_local u64 g_diag_compiling_hash;
} // namespace Shader::Optimization
namespace Vulkan {

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding) {
#ifndef NDEBUG
    Shader::SrtArenaTrace::OriginScope arena_origin{Shader::SrtArenaTrace::Origin::Synchronous};
#endif
    // FIX-044: translating a shader on the GPU thread holds the guest clocks; a PS4 runs
    // precompiled shaders, so the game never sees this time pass.
    Common::GuestClock::CompilePause hold;
    ++Common::GetWorkCounters().shaders_compiled;
    RYDIAG_SCOPE(ShaderCompile, info.pgm_hash);
    last_spv_hash = 0;
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");

    Shader::Optimization::g_diag_compiling_code = code;
    Shader::Optimization::g_diag_compiling_hash = info.pgm_hash;
#ifndef NDEBUG
    const auto translate_start = std::chrono::steady_clock::now();
#endif
    std::vector<u32> spv;
    try {
        // FIX-034: a shader the translator cannot handle no longer stops the emulator. Its
        // draws are skipped and it is reported once, so one run lists every such shader.
        Common::RecoverableScope recoverable;
        const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
        spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    } catch (const Common::RecoverableFailure& e) {
        LOG_ERROR(Render_Vulkan,
                  "SHADER-FAIL: {} shader {:#x} (permutation {}, {} dwords) could not be "
                  "translated: {}; draws that use it are skipped",
                  info.hw_stage, info.pgm_hash, perm_idx, code.size(), e.what());
        DumpFailedShader(code, info.pgm_hash, info.hw_stage);
        return {};
    } catch (const std::bad_alloc&) {
        ReportShaderAllocationFailure(info.hw_stage, info.pgm_hash, info.pgm_base, code.size(),
                                      "synchronous translation");
        return {};
    }
#ifndef NDEBUG
    const auto translate_end = std::chrono::steady_clock::now();
#endif
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");
    last_spv_hash = XXH3_64bits(spv.data(), spv.size() * sizeof(u32));
    if (!Common::PerfFeatureEnabled(44)) {
        // FIX-043 off (-DisablePerf 44): permutations keyed on every resource property.
        info.resource_usage_known = false;
    }

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice(), instance.IsShaderObjectEnabled());
    } else {
        module = CompileSPV(spv, instance.GetDevice(), instance.IsShaderObjectEnabled());
    }
#ifndef NDEBUG
    // PERF-DIAG-013: where runtime shader compile time goes.
    LOG_WARNING(
        Render_Vulkan, "Shader {} {:#x}: translate {:.1f} ms, module {:.1f} ms, {} words",
        info.hw_stage, info.pgm_hash,
        std::chrono::duration<double, std::milli>(translate_end - translate_start).count(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - translate_end)
            .count(),
        spv.size());
#endif

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    // The cache writer now takes ownership instead of copying. Finish optional
    // shader collection before handing the SPIR-V allocation to that worker.
    RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);
    return module;
}

PipelineCache::StoredListing PipelineCache::ReadStoredListing(u64 pgm_hash) {
    return ReadStoredListing(pgm_hash, Storage::DataBase::Instance());
}

static size_t UnoccupiedShaderSlot(u64 pgm_hash, size_t candidate) {
    auto& store = Storage::DataBase::Instance();
    if (!store.IsOpened()) return candidate;
    // Listings intentionally stop after gaps; probe the actual proposed slot
    // so a sparse foreign-engine entry cannot be overwritten.
    return FindUnusedStoredSlot(candidate, [&](size_t index) {
        return store.Exists(Storage::BlobType::ShaderMeta,
                            fmt::format("{:#018x}", HashCombine(pgm_hash, index))) ||
               store.Exists(Storage::BlobType::ShaderBinary,
                            fmt::format("{:#018x}_{}", pgm_hash, index));
    });
}

PipelineCache::StoredListing PipelineCache::ReadStoredListing(u64 pgm_hash,
                                                              Storage::DataBase& store) {
    // Each permutation is stored as <perm hash>.meta (key and shader info) and
    // <program hash>_<index>.spv; indices are dense from 0 (PERF-013 numbers runtime ones after
    // the stored ones), so the listing stops after a run of missing indices. Only file reads, so
    // the PERF-038 reader thread can do it too.
    StoredListing listing{.pgm_hash = pgm_hash};
    u32 missing = 0;
    for (size_t perm_idx = 0; perm_idx < 512 && missing < 8; ++perm_idx) {
        const auto name = fmt::format("{:#018x}", HashCombine(pgm_hash, perm_idx));
        if (!store.Exists(Storage::BlobType::ShaderMeta, name)) {
            ++missing;
            continue;
        }
        missing = 0;
        auto& entry = listing.entries.emplace_back();
        entry.perm_idx = perm_idx;
        store.Load(Storage::BlobType::ShaderMeta, name, entry.meta);
        entry.has_spv = store.Exists(Storage::BlobType::ShaderBinary,
                                     fmt::format("{:#018x}_{}", pgm_hash, perm_idx));
    }
    return listing;
}

void PipelineCache::ApplyStoredListing(StoredListing&& listing, Program& program) {
    static u64 programs = 0;
    static u64 listed = 0;
    static u64 stale = 0;
    const u64 pgm_hash = listing.pgm_hash;
    for (auto& entry : listing.entries) {
        const size_t perm_idx = entry.perm_idx;
        // A metadata slot belongs to the store even if this renderer cannot
        // read its version or profile. Never overwrite the other engine's data.
        auto& occupied_end = stored_perm_end[pgm_hash];
        occupied_end = NextUnusedStoredSlot(occupied_end, perm_idx);
        auto info = std::make_unique<Shader::Info>();
        Shader::StageSpecialization spec{};
        spec.info = info.get();
        size_t stored_idx{};
        Serialization::Archive ar{std::move(entry.meta)};
        // Entries of another format version, cut short, or for another program are skipped.
        bool valid = false;
        try {
            Common::RecoverableScope recoverable;
            valid = ar.SizeBytes() >= 24 && LoadShaderMeta(ar, *info, spec, stored_idx);
        } catch (const Common::RecoverableFailure&) {
            valid = false;
        }
        if (!valid || stored_idx != perm_idx || info->pgm_hash != pgm_hash) {
            ++stale;
            continue;
        }
        // D10: stored before D2, the byte uses_direct takes was padding.
        info->uses_direct = std::ranges::any_of(info->buffers, [](const auto& buffer) {
            return buffer.buffer_type == Shader::BufferType::DirectTable;
        });
        auto& end = stored_perm_end[pgm_hash];
        end = std::max(end, perm_idx + 1);
        // FIX-018/020: translated before a translator fix this program needs.
        if (const auto fixed = else_scope_fixed.find(pgm_hash);
            fixed != else_scope_fixed.end() && perm_idx < fixed->second.boundary) {
            continue;
        }
        if (!entry.has_spv) {
            continue;
        }
        if (perm_idx < program.modules.size() && program.modules[perm_idx].info) {
            continue;
        }
        program.InsertPermut({}, std::move(spec), std::move(info), perm_idx);
        program.modules[perm_idx].stored = true;
        ++listed;
    }
    if (++programs % 500 == 0) {
        LOG_WARNING(Render_Vulkan,
                    "PERF-032: shader store: {} programs looked up, {} stored permutations "
                    "listed, {} entries of another version skipped",
                    programs, listed, stale);
    }
}

void PipelineCache::ListStoredPermutations(u64 pgm_hash, Program& program) {
    ApplyStoredListing(ReadStoredListing(pgm_hash), program);
}

// PERF-038: one stored pipeline, read and parsed by the reader thread.
struct PipelineCache::PrewarmRecord {
    GraphicsPipelineKey key{};
    GraphicsPipeline::SerializationSupport sdata{};
    std::array<u64, MaxShaderStages> pgm{};
    std::array<size_t, MaxShaderStages> perm{};
    std::array<std::shared_ptr<const std::vector<u32>>, MaxShaderStages> spv{};
    std::vector<std::shared_ptr<const StoredListing>> listings;
};

void PipelineCache::PrewarmRead(std::stop_token stop) {
    Common::SetCurrentThreadName("shadGT:PrewarmRead");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
#ifdef RYTY_GPU
    // PERF-102: the game waits on the splash screen until the precompile is done.
    if (!g_precompile_hold.exchange(true)) {
        Common::GuestClock::TakeHold();
        LOG_WARNING(Render_Vulkan, "PERF-102: the game waits on the splash screen for the precompile");
    }
#endif
    auto& store = Storage::DataBase::Instance();
    RebuildFromPrograms(); // PB-1
    // The profile file is written only when missing (by the removed precompile), so it can be
    // older than most entries: GT Sport's is from October 6, the store's entries from October 6
    // to 9. A draw's first use loads the same stored SPIR-V without checking it (PERF-032), so
    // a background build makes the pipeline that draw would make; differences are only logged.
    std::vector<u8> stored_profile;
    if (store.Exists(Storage::BlobType::ShaderProfile, "profile")) {
        store.Load(Storage::BlobType::ShaderProfile, "profile", stored_profile);
    }
    if (!stored_profile.empty()) {
        std::string differences;
        const auto* current = reinterpret_cast<const u8*>(&profile);
        for (size_t i = 0; i < std::min(stored_profile.size(), sizeof(profile)); ++i) {
            if (stored_profile[i] != current[i]) {
                differences += fmt::format(" [{}] {:#x}->{:#x}", i, stored_profile[i], current[i]);
            }
        }
        if (!differences.empty() || stored_profile.size() != sizeof(profile)) {
            LOG_WARNING(Render_Vulkan,
                        "PERF-038: the store's profile file ({} bytes) differs from this "
                        "profile ({} bytes):{}",
                        stored_profile.size(), sizeof(profile), differences);
        }
    }
    auto keys = store.ListBlobs(Storage::BlobType::PipelineKey);
    std::erase_if(keys, [](const auto& key) { return !key.first.starts_with("g_"); });
    // Most recently used first: a pipeline's key is stored again each session that uses it.
    std::ranges::sort(keys, [](const auto& a, const auto& b) { return a.second > b.second; });
    if (const char* limit = std::getenv("SHADGT_PREWARM_LIMIT"); limit && *limit) {
        keys.resize(std::min<size_t>(keys.size(), std::strtoull(limit, nullptr, 10)));
    }
#ifndef NDEBUG
    prewarm_total = keys.size();
#endif
    g_precompile_total = keys.size(); // PERF-098
    g_precompile_done = 0;
    LOG_WARNING(Render_Vulkan, "PERF-038: building {} stored pipelines in the background",
                keys.size());
    const auto read_start = std::chrono::steady_clock::now();
    // miniz uses one mutable archive cursor. Independent files can be read in parallel.
    const unsigned readers = EmulatorSettings.IsPipelineCacheArchived() ? 1u :
        CompilerWorkerBudget::StartupPipelineWorkers(std::thread::hardware_concurrency());
    const size_t window = std::max<size_t>(32, size_t{readers} * 2);
    LOG_INFO(Render_Vulkan, "PRECOMPILE-READ: {} file readers, {}-record window", readers, window);
    u64 skipped = 0;
    // Many pipeline keys reference the same stages. Share immutable reads within
    // this startup, including concurrent requests, with bounded cache retention.
    BoundedReadCache<u64, StoredListing> listing_reads{32_MB, 128};
    BoundedReadCache<u64, std::vector<u32>> binary_reads{32_MB, 256};
    struct MetaIdentity { bool valid{}; u64 pgm{}; size_t perm{}; };
    BoundedReadCache<u64, MetaIdentity> meta_reads{128_KB, 2048};
    const bool complete = OrderedParallelRead(keys.size(), readers, window, stop,
      [&](size_t index) {
        const auto& name = keys[index].first;
        auto record = std::make_unique<PrewarmRecord>();
        bool valid = false;
        try {
            Common::RecoverableScope recoverable;
            std::vector<u8> blob;
            store.Load(Storage::BlobType::PipelineKey, name, blob);
            valid = LoadStoredGraphicsPipeline(std::move(blob), record->key, record->sdata);
            // Only pipelines whose every stage input is stored (see InstallPrewarmRecord).
            valid = valid && record->key.stage_hashes[u32(SwStage::Vertex)] != 0 &&
                    record->key.stage_hashes[u32(SwStage::Fragment)] != 0;
            for (u32 stage = 0; valid && stage < MaxShaderStages; ++stage) {
                const u64 perm_hash = record->key.stage_hashes[stage];
                if (perm_hash == 0) {
                    continue;
                }
                // Only the program hash and index: the full decode is the GPU thread's
                // (ApplyStoredListing), as it registers SRT walker code.
                const auto identity = meta_reads.Get(perm_hash, [&] {
                    std::vector<u8> blob;
                    store.Load(Storage::BlobType::ShaderMeta, fmt::format("{:#018x}", perm_hash), blob);
                    MetaIdentity value;
                    value.valid = blob.size() >= 24 &&
                        PeekShaderMeta(std::move(blob), value.pgm, value.perm) &&
                        HashCombine(value.pgm, value.perm) == perm_hash;
                    if (!value.valid) throw std::runtime_error("incomplete stored shader identity");
                    return value;
                }, [](const auto&) { return sizeof(MetaIdentity); });
                if (!identity->valid) {
                    valid = false;
                    break;
                }
                const auto pgm_hash = identity->pgm;
                const auto perm_idx = identity->perm;
                record->pgm[stage] = pgm_hash;
                record->perm[stage] = perm_idx;
                if (std::ranges::none_of(record->listings, [pgm_hash](const auto& listing) {
                        return listing->pgm_hash == pgm_hash;
                    })) {
                    record->listings.push_back(listing_reads.Get(pgm_hash,
                        [&] { return ReadStoredListing(pgm_hash); }, [](const auto& listing) {
                            size_t bytes = sizeof(listing) + listing.entries.capacity() * sizeof(StoredListing::Entry);
                            for (const auto& entry : listing.entries) bytes += entry.meta.capacity();
                            return bytes;
                        }));
                }
                record->spv[stage] = binary_reads.Get(perm_hash, [&] {
                    std::vector<u32> words;
                    store.Load(Storage::BlobType::ShaderBinary,
                               fmt::format("{:#018x}_{}", pgm_hash, perm_idx), words);
                    // A repair write can still be queued. Do not memoize a
                    // transient missing/partial file for the rest of startup.
                    if (!IsCompleteSpirv(words)) throw std::runtime_error("incomplete stored SPIR-V");
                    return words;
                }, [](const auto& words) { return sizeof(words) + words.capacity() * sizeof(u32); });
            }
        } catch (const std::exception&) {
            // RecoverableFailure, or a corrupt entry (std::bitset rejects bad text).
            valid = false;
        }
        if (!valid) return std::unique_ptr<PrewarmRecord>{};
        return record;
      }, [&](size_t, std::unique_ptr<PrewarmRecord> record) {
#ifndef NDEBUG
        ++prewarm_read;
#endif
        if (!record) { ++skipped; ++g_precompile_done; return; }
        std::unique_lock lk{prewarm_mutex};
        prewarm_cv.wait(lk, stop, [this] { return prewarm_records.size() < 32; });
        if (stop.stop_requested()) return;
        prewarm_records.push_back(std::move(record));
        ++prewarm_queued;
      }, [] {
        Common::SetCurrentThreadName("shadGT:PrewarmLoad");
        Common::AllowAllCores();
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Normal);
      });
    if (!complete || stop.stop_requested()) return;
    LOG_INFO(Render_Vulkan, "PRECOMPILE-READ: {} records prepared in {:.3f} s", keys.size(),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - read_start).count());
    LOG_WARNING(Render_Vulkan,
                "PERF-038: reader done: {} stored pipelines read, {} skipped (other key version, "
                "missing stages or files)",
                keys.size(), skipped);
    prewarm_reader_done = true;
    if (keys.empty()) {
        ReleasePrecompileHold(); // nothing stored yet (first start)
    }
}

void PipelineCache::IdlePrewarm(bool splash) {
    if (!prewarm_enabled) {
        ReleasePrecompileHold();
        return;
    }
    // PERF-102: released once every stored pipeline is built (or after 15 minutes at most).
    static const auto first_call = std::chrono::steady_clock::now();
    if (PrecompileHoldsGame() &&
        ((prewarm_reader_done && !GetPrecompileProgress().active) ||
         std::chrono::steady_clock::now() - first_call > std::chrono::minutes{15})) {
        ReleasePrecompileHold();
    }
    if (build_workers) {
        build_workers->SetBoost(splash);
    } else if (splash) {
        build_workers = std::make_unique<PipelineBuildWorkers>();
        build_workers->SetBoost(true);
    }
    std::scoped_lock lk{lookup_mutex};
    InstallPrewarmed(splash);
}

void PipelineCache::InstallPrewarmed(bool force) {
    if (!prewarm_enabled) {
        return;
    }
    using namespace std::chrono_literals;
    const auto start = std::chrono::steady_clock::now();
    ReportPrewarm(start);
    // Not while new pipelines are being needed: the draws come first (PERF-098: unless forced,
    // while the splash screen shows).
    if (prewarm_queued.load(std::memory_order_relaxed) == 0 ||
        (!force && start - last_pipeline_miss < 2s)) {
        return;
    }
    // The members a key is computed in are restored, so the draw sees what it would have.
    const auto saved_key = S().graphics_key;
    const auto saved_infos = S().infos;
    const auto saved_modules = S().modules;
    const auto saved_runtime_infos = S().runtime_infos;
    auto* const saved_fetch = S().fetch_shader;
    const auto budget = start + (force ? 20000us : 300us);
    do {
        std::unique_ptr<PrewarmRecord> record;
        {
            std::scoped_lock lk{prewarm_mutex};
            if (prewarm_records.empty()) {
                break;
            }
            record = std::move(prewarm_records.front());
            prewarm_records.pop_front();
        }
        --prewarm_queued;
        prewarm_cv.notify_one();
        if (InstallPrewarmRecord(*record)) {
#ifndef NDEBUG
            ++prewarm_stats.installed;
#endif
        } else {
#ifndef NDEBUG
            ++prewarm_stats.skipped;
#endif
            ++g_precompile_done; // PERF-098
        }
    } while (std::chrono::steady_clock::now() < budget);
    S().graphics_key = saved_key;
    S().infos = saved_infos;
    S().modules = saved_modules;
    S().runtime_infos = saved_runtime_infos;
    S().fetch_shader = saved_fetch;
#ifndef NDEBUG
    prewarm_stats.ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
#endif
}

bool PipelineCache::InstallPrewarmRecord(PrewarmRecord& record) {
    for (auto& listing : record.listings) {
        auto [it, is_new] = program_cache.try_emplace(listing->pgm_hash);
        if (is_new) {
            it.value() = std::make_unique<Program>();
            // Installation consumes metadata; make one private copy per program,
            // rather than mutating bytes still shared with reader threads.
            ApplyStoredListing(StoredListing{*listing}, *it.value());
        }
    }
    if (graphics_pipelines.contains(record.key) || pending_builds.contains(record.key)) {
        return false;
    }
    const bool tess_emulated = record.key.prim_type == AmdGpu::PrimitiveType::RectList ||
                               record.key.prim_type == AmdGpu::PrimitiveType::QuadList;
    S().infos.fill(nullptr);
    S().modules.fill(nullptr);
    S().fetch_shader = nullptr;
    for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
        if (record.pgm[stage] == 0) {
            continue;
        }
        const auto it = program_cache.find(record.pgm[stage]);
        if (it == program_cache.end() || record.perm[stage] >= it->second->modules.size()) {
            return false;
        }
        auto& permutation = it->second->modules[record.perm[stage]];
        if (!permutation.info) {
            return false;
        }
        if (permutation.stored) {
            // The same SPIR-V a draw's first use would load (PERF-032), read on the reader thread.
            if (!record.spv[stage] || !IsCompleteSpirv(*record.spv[stage])) {
                return false;
            }
            permutation.module = CompileSPV(*record.spv[stage], instance.GetDevice(), instance.IsShaderObjectEnabled());
            permutation.stored = false;
        }
        if (!permutation.module) {
            return false;
        }
        // Tessellation helpers are made from the vertex outputs, which old metadata lacks; a draw
        // recovers them from the guest code, a background build cannot.
        if (tess_emulated && stage == u32(SwStage::Vertex) &&
            !permutation.info->attribute_flags_known) {
            return false;
        }
        // A mip fallback indexed at runtime takes its descriptor count from the draw's T#.
        if (std::ranges::any_of(permutation.info->images, [](const auto& image) {
                return image.mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex;
            })) {
            return false;
        }
        S().infos[stage] = permutation.info.get();
        S().modules[stage] = permutation.module;
        // The runtime info a draw passes is the one the permutation was specialized with.
        S().runtime_infos[stage] = permutation.spec.runtime_info;
        if (auto& fetch = permutation.spec.fetch_shader_data; !fetch.Empty()) {
            S().fetch_shader = &fetch;
        }
    }
    S().graphics_key = record.key;
    auto build = MakePipelineBuild();
    build->preloading = true;
    build->sdata = record.sdata;
    pending_builds.emplace(record.key, build);
    QueueBackgroundBuild(build);
    return true;
}

bool PipelineCache::PreservesDepthOutput() const {
    const auto& regs = Regs();
    if (!regs.depth_control.depth_enable || !regs.depth_control.depth_write_enable ||
        !regs.depth_buffer.DepthValid()) {
        return false;
    }
    // Match RefreshGraphicsKey: disabled color writes ignore stale bound color
    // attachments. Such a draw is still a depth-only bake and cannot be skipped.
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable) {
        return true;
    }
    // Shadow bakes need all depth-only draws, including on their first encounter.
    // Ordinary color rendering also writes depth but must keep its asynchronous policy.
    for (u32 cb = 0; cb < std::size(regs.color_buffers); ++cb) {
        if (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb) != 0) return false;
    }
    return true;
}

static bool LearnedOneOffWait(const AmdGpu::Regs& regs) {
    if (auto observer = CurrentOneOffObserver()) {
        const auto outputs = GetOneOffOutputs(regs);
        for (size_t i = 0; i < outputs.count; ++i) {
            if (observer->MustWait(outputs.keys[i])) return true;
        }
    }
    return false;
}

bool PipelineCache::NeverSkipped(const std::array<const Shader::Info*, MaxShaderStages>& infos) {
    if (const auto* fragment = infos[u32(Shader::SwStage::Fragment)];
        fragment && IsGt7OneShotShader(fragment->pgm_hash)) return true;
    // A loading-time shadow bake may run only once. Preserve depth-only output
    // without forcing ordinary menu color draws to wait for compilation.
    if (PreservesDepthOutput()) {
        return true;
    }
    if (LearnedOneOffWait(Regs())) return true;
    // Keep going: rendering must not wait for a cosmetic pipeline.
    if (CompileMode() == 2) return false;
    // FIX-064: tessellated draws wait for their pipeline. On GT Sport's race summary they are
    // the driver figure; skipped while each of its pipelines built, parts of it blinked out
    // frame after frame. -DisablePerf 88 skips them like other draws.
    // FIX-115: not during a race, though: there a tessellated car or road part that builds for
    // 0.3-2 s stopped the race that long. It shows a moment later instead (the quick wait still
    // draws the ones that build fast). -DisablePerf 138 waits in races too.
    static const bool wait_for_tessellation = Common::PerfFeatureEnabled(88);
    static const bool skip_in_races = Common::PerfFeatureEnabled(138);
#ifdef RYTY_GPU
    if (AsyncMenuPipelines() || (skip_in_races && RytyGtsInRace())) {
        return false;
    }
#endif
    return wait_for_tessellation && infos[u32(Shader::SwStage::TessellationControl)] != nullptr;
}

bool PipelineCache::WritesMemory(const std::array<const Shader::Info*, MaxShaderStages>& infos) {
    // PERF-076: a draw that writes buffers or images is never skipped. GT Sport's results screen
    // writes data later draws read as vertex descriptors; skipping those draws left garbage
    // there and the renderer ran out of memory on it.
    for (const auto* info : infos) {
        if (!info) {
            continue;
        }
        if (info->has_storage_images) {
            return true;
        }
        for (const auto& buffer : info->buffers) {
            if (buffer.is_written) {
                return true;
            }
        }
        for (const auto& image : info->images) {
            if (image.is_written) {
                return true;
            }
        }
    }
    return false;
}

bool PipelineCache::AsyncPipelines() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADGT_ASYNC_PIPELINES");
#ifdef RYTY_GPU
        // PERF-100: GPU.compile_freeze (on by default) pauses the game while a new pipeline
        // compiles instead of skipping its draws.
        // PERF-103: GPU.compile_mode 0 (pause) waits; slow motion and keep going build in
        // the background.
        const bool by_default = CompileMode() != 0;
#else
        const bool by_default = false;
#endif
        const bool on = env ? env[0] != '0' : by_default;
        return on && Common::PerfFeatureEnabled(76);
    }();
    return enabled;
}

void PipelineCache::ReportPrewarm(std::chrono::steady_clock::time_point now) {
#ifndef NDEBUG
    auto& stats = prewarm_stats;
    if (now - stats.since < std::chrono::seconds{2}) {
        return;
    }
    if (async_stats.deferred + async_stats.installed + async_stats.skipped_draws +
            async_stats.skipped_translating !=
        0) {
        LOG_WARNING(Render_Vulkan,
                    "PERF-076 background pipelines in {:.1f} s: {} new ones built without a wait, "
                    "{} ready and installed, {} draws skipped meanwhile; SHD-005: {} draws "
                    "skipped while their shaders translated",
                    std::chrono::duration<double>(now - stats.since).count(),
                    async_stats.deferred, async_stats.installed, async_stats.skipped_draws,
                    async_stats.skipped_translating);
        async_stats = {};
    }
    const u64 built = prewarm_built.load();
    if (stats.installed + stats.hits + stats.cancelled + stats.promoted != 0 ||
        built != stats.built_before) {
        LOG_WARNING(Render_Vulkan,
                    "PERF-038 background builds in {:.1f} s: {} queued ({} skipped, {:.1f} ms on "
                    "this thread), {} built ({} in all), {} used by draws without waiting, {} "
                    "built for a draw instead, {} moved ahead by the read-ahead; {} waiting; "
                    "reader {}/{}{}",
                    std::chrono::duration<double>(now - stats.since).count(), stats.installed,
                    stats.skipped, stats.ms, built - stats.built_before, built, stats.hits,
                    stats.cancelled, stats.promoted,
                    build_workers ? build_workers->BackgroundQueued() : 0, prewarm_read.load(),
                    prewarm_total.load(), prewarm_reader_done.load() ? " (done)" : "");
    }
    const u64 built_now = built;
    stats = {};
    stats.built_before = built_now;
    stats.since = now;
#else
    (void)now;
#endif
}

vk::ShaderModule PipelineCache::LoadStoredModule(u64 pgm_hash, size_t perm_idx) {
    return LoadStoredModule(pgm_hash, perm_idx, Storage::DataBase::Instance());
}

vk::ShaderModule PipelineCache::LoadStoredModule(u64 pgm_hash, size_t perm_idx,
                                                 Storage::DataBase& store) {
    static u64 loaded = 0;
    std::vector<u32> spv;
    store.Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx), spv);
    if (!IsCompleteSpirv(spv)) {
        static u32 reports = 0;
        if (reports++ < 20) {
            LOG_WARNING(Render_Vulkan,
                        "PERF-032: stored shader {:#x} permutation {} is incomplete; translating "
                        "it again",
                        pgm_hash, perm_idx);
        }
        return {};
    }
    // FIX-GT7-011: FIX-226 changed how float atomic min/max are made without
    // VK_EXT_shader_atomic_float2, and the stored translations kept their version: one made the
    // other way than RYGT_FLOAT_ATOMIC_EXACT asks is translated again (GT7's occlusion depth
    // pyramid came out wrong from the old way).
    if (const auto emulation = FindFloatAtomicEmulation(spv);
        emulation != FloatAtomicEmulation::None &&
        emulation != (Shader::Backend::SPIRV::FloatAtomicExact()
                          ? FloatAtomicEmulation::OneAtomic
                          : FloatAtomicEmulation::BothAtomics)) {
        static u32 reports = 0;
        if (reports++ < 20) {
            LOG_WARNING(Render_Vulkan,
                        "FIX-GT7-011: stored shader {:#x} permutation {} makes its float atomic "
                        "min/max the other way; translating it again",
                        pgm_hash, perm_idx);
        }
        return {};
    }
    if (++loaded % 500 == 0) {
        LOG_WARNING(Render_Vulkan,
                    "PERF-032: {} shader S().modules loaded from the store instead of translated",
                    loaded);
    }
    return CompileSPV(spv, instance.GetDevice(), instance.IsShaderObjectEnabled());
}

void PipelineCache::DumpFailedShader(std::span<const u32> code, u64 hash, Shader::HwStage stage) {
    // FIX-034: the guest code of a shader that could not be translated, raw and as a listing,
    // in the log folder (once per program), so it can be studied and tested offline.
    if (!failed_shaders.insert(hash).second) {
        return;
    }
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "failed_shaders";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto name = fmt::format("{}_{:#x}", stage, hash);
    Common::FS::IOFile{dir / (name + ".bin"), Common::FS::FileAccessMode::Create}.WriteSpan(code);
    std::ofstream{dir / (name + ".txt")} << Shader::ListGcnCode(code);
}

#ifndef NDEBUG
bool IsWatchedShader(u64 hash) {
    static const std::vector<u64> watched = [] {
        // The Nurburgring grass: vertex generator, blade generator, and the pass before them.
        // DIAG-039: the dealership shader whose buffer V#s read as float constants.
        std::vector<u64> hashes{0xab6a2d10, 0xaa3822a3, 0x766d0f18, 0xae32f77f};
        if (const char* env = Common::DiagEnv("SHADGT_WATCH_SHADERS"); env && *env) {
            hashes.clear();
            const std::string list{env};
            for (size_t start = 0; start < list.size();) {
                const size_t end = std::min(list.find(',', start), list.size());
                hashes.push_back(std::stoull(list.substr(start, end - start), nullptr, 16));
                start = end + 1;
            }
        }
        return hashes;
    }();
    return std::ranges::find(watched, hash) != watched.end();
}
#else
// FIX-017: these shaders must retain the per-use buffer freshness path in Release.
// The diagnostic environment override is available only in Debug.
bool IsWatchedShader(u64 hash) {
    return hash == 0xab6a2d10 || hash == 0xaa3822a3 ||
           hash == 0x766d0f18 || hash == 0xae32f77f;
}
#endif

void PipelineCache::LoadElseScopeFixed() {
    const auto& serial = Common::Singleton<Common::ElfInfo>::Instance()->GameSerial();
    const auto& store = Storage::DataBase::Instance();
    else_scope_fixed_path = VideoCore::ShaderStoreMarksPath(
        Common::FS::GetUserPath(Common::FS::PathType::CacheDir), serial, store.StorePath(),
        store.UsesCompatibleSibling());
    std::ifstream file{else_scope_fixed_path};
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream fields{line};
        u64 hash{};
        size_t boundary{};
        u32 fixes = 1; // Lines written before FIX-020 have no mask.
        if (fields >> std::hex >> hash >> std::dec >> boundary) {
            fields >> fixes;
            else_scope_fixed[hash] = {boundary, fixes};
        }
    }
    if (!else_scope_fixed.empty()) {
        LOG_INFO(Render_Vulkan, "FIX-018: {} programs already translated with the else-scope fix",
                 else_scope_fixed.size());
    }
}

void PipelineCache::SaveElseScopeFixed() const {
    if (else_scope_fixed_path.empty()) {
        return;
    }
    std::ofstream file{else_scope_fixed_path, std::ios::trunc};
    for (const auto& [hash, program] : else_scope_fixed) {
        file << std::hex << hash << ' ' << std::dec << program.boundary << ' ' << program.fixes
             << '\n';
    }
}

// FIX-058: GT Sport's race summary draws tessellated objects whose tessellation constants are
// garbage (strides of hundreds of MB). Hull shaders translated from them declared arrays of 200
// million entries, and the driver ran out of memory compiling their pipelines (the process went
// from 11 GB to 100 GB in seconds; the driver figure flickered while they built). Constants are
// sane when they fit the 64 KB of LDS they describe; draws with others are skipped.
// -DisablePerf 82 draws them.
#ifndef NDEBUG
// DIAG-063: the tessellation-constant V#s (HS user data 7..10) an indirect buffer sets, read
// from a copy of it.
static std::vector<VAddr> TessBasesInIb(const u32* data, u32 dwords) {
    std::vector<u32> ib(dwords);
    Common::CopyGuest(ib.data(), data, ib.size() * sizeof(u32));
    std::vector<VAddr> bases;
    std::array<u32, 16> hs{};
    constexpr u32 HsUserData = 0x2D0C;
    const auto apply = [&](u32 first, std::span<const u32> values) {
        bool touched = false;
        for (u32 i = 0; i < values.size(); ++i) {
            if (first + i >= HsUserData + 7 && first + i < HsUserData + 11) {
                hs[first + i - HsUserData] = values[i];
                touched = true;
            }
        }
        if (touched && hs[9] == 3) {
            bases.push_back((VAddr(hs[8] & 0xffff) << 32) | hs[7]);
        }
    };
    std::span<const u32> rest{ib};
    while (!rest.empty()) {
        const u32 type = rest.front() >> 30;
        if (type == 2) {
            rest = rest.subspan(1);
        } else if (type == 0) {
            const u32 first = rest.front() & 0xffff;
            const u32 count = ((rest.front() >> 16) & 0x3fff) + 1;
            if (count + 1 > rest.size()) {
                break;
            }
            apply(first, rest.subspan(1, count));
            rest = rest.subspan(count + 1);
        } else if (type == 3) {
            const u32 count = ((rest.front() >> 16) & 0x3fff) + 1;
            const u32 opcode = (rest.front() >> 8) & 0xff;
            if (count + 1 > rest.size()) {
                break;
            }
            if (opcode == 0x76 && count >= 1) { // SET_SH_REG
                apply(0x2C00 + (rest[1] & 0xffff), rest.subspan(2, count - 1));
            }
            rest = rest.subspan(count + 1);
        } else {
            break;
        }
    }
    return bases;
}

// DIAG-063: whether the indirect buffer a draw came from changes after it was decoded, and the
// constant V#s it sets before and after.
static void WatchIbAfterDecode(u32 n, bool sane, VAddr base) {
    const auto [ib, dwords] = AmdGpu::Liverpool::CurrentDecodeIb();
    if (!ib || dwords == 0 || dwords > 4'000'000) {
        LOG_WARNING(Render_Vulkan, "DIAG-063: {} draw {}: not inside an indirect buffer",
                    sane ? "sane" : "garbage", n);
        return;
    }
    std::vector<u32> before(dwords);
    Common::CopyGuest(before.data(), ib, before.size() * sizeof(u32));
    const auto bases_before = TessBasesInIb(ib, dwords);
    static std::atomic<u32> dumps{};
    if ((!sane || n == 5000) && dumps.fetch_add(1) < 6) {
        // DIAG-064: the indirect buffer and 64 KB around the constants, for offline study.
        const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
        const auto name = fmt::format("tess_{}_{}", sane ? "sane" : "garbage", n);
        if (std::ofstream f{dir / (name + "_ib.bin"), std::ios::binary}) {
            f.write(reinterpret_cast<const char*>(before.data()), before.size() * sizeof(u32));
        }
        std::vector<u8> around(64_KB);
        const VAddr from = base - 32_KB;
        Common::CopyGuest(around.data(), reinterpret_cast<const void*>(from), around.size());
        if (std::ofstream f{dir / (name + "_mem.bin"), std::ios::binary}) {
            f.write(reinterpret_cast<const char*>(around.data()), around.size());
        }
        LOG_WARNING(Render_Vulkan, "DIAG-064: {} draw {}: indirect buffer {:#x} ({} dwords) and "
                                   "memory from {:#x} saved as {}_*.bin",
                    sane ? "sane" : "garbage", n, reinterpret_cast<VAddr>(ib), dwords, from,
                    name);
    }
    std::thread([n, sane, base, ib, dwords, before = std::move(before), bases_before] {
        std::string timeline;
        for (const u32 ms : {1u, 3u, 10u, 30u}) {
            std::this_thread::sleep_until(std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(ms));
            std::vector<u32> now(dwords);
            Common::CopyGuest(now.data(), ib, now.size() * sizeof(u32));
            size_t first_diff = dwords;
            for (size_t i = 0; i < dwords; ++i) {
                if (now[i] != before[i]) {
                    first_diff = i;
                    break;
                }
            }
            std::array<u32, 7> c{};
            Common::CopyGuest(c.data(), reinterpret_cast<const void*>(base), sizeof(c));
            const bool constants_sane =
                c[2] != 0 && std::ranges::all_of(c, [](u32 x) { return x <= 64_KB; });
            timeline += fmt::format(" | +{} ms: ib {}, constants {}", ms,
                                    first_diff == dwords
                                        ? std::string("same")
                                        : fmt::format("changed from dword {}", first_diff),
                                    constants_sane ? "sane" : "garbage");
        }
        const auto bases_after = TessBasesInIb(ib, dwords);
        const auto list = [](const std::vector<VAddr>& v) {
            std::string out;
            for (size_t i = 0; i < std::min<size_t>(v.size(), 8); ++i) {
                out += fmt::format(" {:#x}", v[i]);
            }
            return out + (v.size() > 8 ? fmt::format(" (+{})", v.size() - 8) : "");
        };
        LOG_WARNING(Render_Vulkan,
                    "DIAG-063: {} draw {} (constants {:#x}) in indirect buffer {:#x} ({} "
                    "dwords){}; constant V#s in it at decoding:{}; 30 ms later:{}",
                    sane ? "sane" : "garbage", n, base, reinterpret_cast<VAddr>(ib), dwords,
                    timeline, list(bases_before), list(bases_after));
    }).detach();
}
#endif

static bool SaneTessConstants(const Shader::Info& hull, const AmdGpu::Liverpool* liverpool,
                              bool read_ahead) {
    static const bool check = Common::PerfFeatureEnabled(82);
    if (!check || hull.tess_consts_dword_offset < 0) {
        return true;
    }
    const auto buf = hull.ReadUdReg<AmdGpu::Buffer>(static_cast<u32>(hull.tess_consts_ptr_base),
                                                    static_cast<u32>(hull.tess_consts_dword_offset));
    const VAddr base = buf.base_address;
    Shader::TessellationDataConstantBuffer c{};
#ifdef RYTY_GPU
    if (base == 0 || !Core::RytyMemory::IsGuestReadable(base, sizeof(c))) {
        return false;
    }
#endif
    constexpr u32 Lds = 64_KB;
    const auto is_sane = [&] {
        return c.ls_stride <= Lds && c.hs_cp_stride <= Lds && c.num_patches != 0 &&
               c.num_patches <= Lds && c.hs_output_base <= Lds && c.patch_const_size <= Lds &&
               c.patch_const_base <= Lds && c.patch_output_size <= Lds;
    };
    Common::CopyGuest(&c, reinterpret_cast<const void*>(base), sizeof(c));
    const bool sane = is_sane();
#ifndef NDEBUG
    if (DrawPreparationEnabled(4)) {
        // DIAG-060: sane and garbage constants per hull program, every 5 s.
        static thread_local std::unordered_map<u64, std::pair<u32, u32>> per_hull;
        static thread_local u32 garbage_drawn{};
        static thread_local u32 garbage_read_ahead{};
        if (!sane) {
            ++(read_ahead ? garbage_read_ahead : garbage_drawn);
        }
        static thread_local auto last = std::chrono::steady_clock::now();
        auto& [good, bad] = per_hull[hull.pgm_hash];
        ++(sane ? good : bad);
        if (PollPreparationDiagnostics())
        if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
            last = now;
            std::string line;
            for (const auto& [hash, counts] : per_hull) {
                if (counts.second != 0) {
                    line += fmt::format(" {:#x}={}/{}", hash, counts.first, counts.second);
                }
            }
            if (!line.empty()) {
                LOG_WARNING(Render_Vulkan,
                            "DIAG-060: hulls with garbage constants (sane/garbage):{}; decoding "
                            "submission {} of {}; garbage in {} draws, {} read-ahead states",
                            line, liverpool ? liverpool->DecodingSubmit() : 0,
                            liverpool ? liverpool->SubmittedCount() : 0, garbage_drawn,
                            garbage_read_ahead);
                garbage_drawn = 0;
                garbage_read_ahead = 0;
            }
            per_hull.clear();
        }
    }
    else {
        // DIAG-060: sane and garbage constants per hull program, every 5 s.
        static std::mutex stats_mutex;
        static std::unordered_map<u64, std::pair<u32, u32>> per_hull;
        static u32 garbage_drawn{};
        static u32 garbage_read_ahead{};
        if (!sane) {
            ++(read_ahead ? garbage_read_ahead : garbage_drawn);
        }
        static auto last = std::chrono::steady_clock::now();
        std::scoped_lock lk{stats_mutex};
        auto& [good, bad] = per_hull[hull.pgm_hash];
        ++(sane ? good : bad);
        if (PollPreparationDiagnostics())
        if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
            last = now;
            std::string line;
            for (const auto& [hash, counts] : per_hull) {
                if (counts.second != 0) {
                    line += fmt::format(" {:#x}={}/{}", hash, counts.first, counts.second);
                }
            }
            if (!line.empty()) {
                LOG_WARNING(Render_Vulkan,
                            "DIAG-060: hulls with garbage constants (sane/garbage):{}; decoding "
                            "submission {} of {}; garbage in {} draws, {} read-ahead states",
                            line, liverpool ? liverpool->DecodingSubmit() : 0,
                            liverpool ? liverpool->SubmittedCount() : 0, garbage_drawn,
                            garbage_read_ahead);
                garbage_drawn = 0;
                garbage_read_ahead = 0;
            }
            per_hull.clear();
        }
    }
#ifdef RYTY_GPU
    if (sane && !read_ahead) {
        // DIAG-067: who writes these constants next, and when (SHADGT_WATCH_TESS=<ms>).
        static const u32 watch_ms = [] {
            const char* env = Common::DiagEnv("SHADGT_WATCH_TESS");
            return env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 0u;
        }();
        static auto next_watch = std::chrono::steady_clock::now();
        if (watch_ms != 0 && std::chrono::steady_clock::now() >= next_watch) {
            next_watch = std::chrono::steady_clock::now() + std::chrono::milliseconds{watch_ms + 50};
            RytyWatchWrites(base, watch_ms,
                            fmt::format("tessellation constants of hs {:#x} decoded in submission "
                                        "{}; {}",
                                        hull.pgm_hash, liverpool ? liverpool->DecodingSubmit() : 0,
                                        AmdGpu::Liverpool::DescribeEops()));
        }
    }
#endif
    if (sane) {
        // DIAG-059: what sane constants and their V#s look like, to compare with garbage ones.
        static std::atomic<u32> sane_logged{};
        static thread_local u32 thread_sane_logged{};
        const u32 n = DrawPreparationEnabled(8) ? ++thread_sane_logged : ++sane_logged;
        if (n <= (DrawPreparationEnabled(8) ? 4u : 40u) || n % 5000 == 0) {
            if (n % 5000 == 0 && std::getenv("SHADGT_TESS_SUBMIT_CHECK")) {
                WatchIbAfterDecode(n, true, base);
            }
            const auto dump = liverpool ? liverpool->FindRecentConstDump(base, sizeof(c))
                                        : std::nullopt;
            const auto rec = AmdGpu::Liverpool::FindTessSubmit(base);
            LOG_WARNING(Render_Vulkan, "DIAG-059: sane constants {}: {}; DIAG-061: {} at "
                                       "submission {}, decoding {} of {}; DIAG-066: {}",
                        n,
                        dump ? fmt::format("CE dump {} of {}", dump->sequence,
                                           liverpool->ConstDumpSequence())
                             : std::string("not from a recent CE dump"),
                        rec ? (rec->sane ? "sane" : "garbage") : "unseen", rec ? rec->submit : 0,
                        liverpool ? liverpool->DecodingSubmit() : 0,
                        liverpool ? liverpool->SubmittedCount() : 0,
                        AmdGpu::Liverpool::DescribeEops());
            LOG_WARNING(Render_Vulkan,
                        "DIAG-059: sane tessellation constants {} of hs {:#x} at {:#x}: ls stride "
                        "{:#x}, cp stride {:#x}, patches {:#x}, output base {:#x}, patch output "
                        "size {:#x}; V# {:08x} {:08x} {:08x} {:08x}",
                        n, hull.pgm_hash, base, c.ls_stride, c.hs_cp_stride, c.num_patches,
                        c.hs_output_base, c.patch_output_size,
                        reinterpret_cast<const u32*>(&buf)[0], reinterpret_cast<const u32*>(&buf)[1],
                        reinterpret_cast<const u32*>(&buf)[2], reinterpret_cast<const u32*>(&buf)[3]);
        }
    }
    if (!sane) {
        static std::atomic<u32> logged{};
        if (const u32 n = ++logged; n <= 60 || n % 100 == 0) {
            if (n <= 40 && std::getenv("SHADGT_TESS_SUBMIT_CHECK")) {
                WatchIbAfterDecode(n, false, base);
            }
            auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
            const bool gpu_written =
                rasterizer && rasterizer->GetBufferCache().IsRegionGpuModified(base, sizeof(c));
            // Where the V# itself was read: a table in memory the user data points to, or the
            // user data registers.
            VAddr table = 0;
            if (u32(hull.tess_consts_ptr_base) != Shader::IR::NumScalarRegs &&
                u32(hull.tess_consts_ptr_base) + 1 < hull.user_data.size()) {
                std::memcpy(&table, &hull.user_data[u32(hull.tess_consts_ptr_base)], sizeof(table));
                table &= 0xFFFFFFFFFFFFULL;
                table += u64(hull.tess_consts_dword_offset) * 4;
            }
            const bool table_gpu_written =
                rasterizer && table != 0 &&
                rasterizer->GetBufferCache().IsRegionGpuModified(table, 16);
            const auto dump = liverpool ? liverpool->FindRecentConstDump(base, sizeof(c))
                                        : std::nullopt;
            LOG_WARNING(Render_Vulkan,
                        "FIX-058: constants of draw {}: {}", n,
                        dump ? fmt::format("CE dump {} of {} (ce {} de {}) at {:#x}+{:#x}",
                                           dump->sequence, liverpool->ConstDumpSequence(),
                                           dump->ce_count, dump->de_count, dump->address,
                                           dump->size)
                             : std::string("not from a recent CE dump"));
            if (liverpool) {
                // DIAG-061: the same constants when the game submitted the draw.
                const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
                const auto cmd = liverpool->FindRecentCmdBuffer(base, sizeof(c));
                const auto rec = AmdGpu::Liverpool::FindTessSubmit(base);
                LOG_WARNING(Render_Vulkan,
                            "DIAG-061: constants of draw {} {}; decoding submission {} of {}; "
                            "inside a recent command buffer: {}; DIAG-062: its indirect buffer "
                            "changed after submission: {}",
                            n,
                            rec ? fmt::format("were {} at submission {} ({:.1f} ms ago, ls stride "
                                              "{:#x}{})",
                                              rec->sane ? "sane" : "garbage", rec->submit,
                                              (now - rec->time_ns) / 1e6, rec->ls_stride,
                                              rec->in_cmd_buffer ? ", in its command buffer" : "")
                                : std::string("were not seen at any submission"),
                            liverpool->DecodingSubmit(), liverpool->SubmittedCount(),
                            cmd ? fmt::format("yes ({} of {})", cmd->sequence,
                                              liverpool->CmdBufferSequence())
                                : std::string("no"),
                            AmdGpu::Liverpool::CurrentIbChangedSinceSubmit());
            }
            // DIAG-065: the renderer's own recent writes over the constants (readbacks, fault
            // flushes, CP writes), newest last.
            LOG_WARNING(Render_Vulkan, "DIAG-065: emulator writes over the constants of draw {}:{}",
                        n, Core::Memory::Instance()->DescribeBackingWrites(base, 0));
            LOG_WARNING(Render_Vulkan, "DIAG-066: draw {} decoded in submission {} of {}; {}", n,
                        liverpool ? liverpool->DecodingSubmit() : 0,
                        liverpool ? liverpool->SubmittedCount() : 0,
                        AmdGpu::Liverpool::DescribeEops());
            LOG_WARNING(Render_Vulkan,
                        "FIX-058: V# of draw {} read from {} {:#x} (GPU-written: {}); user data "
                        "regs {} (ptr base {}, dword {})",
                        n, table ? "memory" : "user data", table, table_gpu_written,
                        hull.user_data.size(), u32(hull.tess_consts_ptr_base),
                        hull.tess_consts_dword_offset);
            LOG_WARNING(Render_Vulkan,
                        "FIX-058: skipping tessellated {} {} of hs {:#x}: constants at {:#x} "
                        "are garbage (ls stride {:#x}, cp stride {:#x}, patches {:#x}, output "
                        "base {:#x}, patch output size {:#x}); GPU-written: {}; V# {:08x} "
                        "{:08x} {:08x} {:08x}",
                        read_ahead ? "read-ahead state" : "draw", n, hull.pgm_hash, base,
                        c.ls_stride, c.hs_cp_stride, c.num_patches,
                        c.hs_output_base, c.patch_output_size, gpu_written,
                        reinterpret_cast<const u32*>(&buf)[0], reinterpret_cast<const u32*>(&buf)[1],
                        reinterpret_cast<const u32*>(&buf)[2], reinterpret_cast<const u32*>(&buf)[3]);
        }
    }
#else
    if (!sane) {
        static std::atomic<u32> logged{};
        if (const u32 n = ++logged; n <= 60 || n % 100 == 0) {
            LOG_WARNING(Render_Vulkan, "FIX-058: skipping tessellated {} of hs {:#x}: invalid constants at {:#x}",
                        read_ahead ? "read-ahead state" : "draw", hull.pgm_hash, base);
        }
    }
#endif
    return sane;
}

bool PipelineCache::SkipTranslationWait(SwStage sw_stage, const Shader::ShaderParams& params) {
    // SHD-005: as a race's draws are skipped while their new pipeline builds (PERF-076), a draw
    // whose new shader still translates on a worker is skipped too, instead of the command thread
    // waiting for it (GT Sport's first race on a track stopped for 100-300 ms whenever a few
    // dozen new shaders came at once). Not compute, and not a shader that writes memory (later
    // work reads what it writes). -DisablePerf 171 waits as before.
    static const bool enabled = Common::PerfFeatureEnabled(171);
    if (!enabled || PreservesDepthOutput() || IsGt7OneShotShader(params.hash) ||
        regs_override != nullptr || sw_stage == SwStage::Compute ||
        LearnedOneOffWait(Regs()) ||
        !AsyncTranslation() || !AsyncPipelines() || MenusWait()) {
        return false;
    }
    auto [it, inserted] = code_writes_memory.try_emplace(params.hash, false);
    if (inserted) {
        it.value() = Shader::HasMemoryWrites(params.code);
    }
    return !it->second;
}

bool PipelineCache::AsyncTranslation() const {
    // FIX-078: -DisablePerf 102 translates on the command thread as before. Dumped, collected or
    // patched shaders need their permutation index while they are translated.
    static const bool enabled = Common::PerfFeatureEnabled(102) &&
 #ifndef NDEBUG
                                !EmulatorSettings.IsDumpShaders() &&
 #endif
                                !EmulatorSettings.IsShaderCollect() &&
                                !EmulatorSettings.IsPatchShaders();
    return enabled && translate_workers != nullptr;
}

bool PipelineCache::QueueTranslation(HwStage hw_stage, SwStage sw_stage,
                                     const Shader::ShaderParams& params,
                                     const Shader::RuntimeInfo& runtime_info,
                                     const Shader::Backend::Bindings& start, bool speculative) {
#ifndef NDEBUG
    Shader::SrtArenaTrace::OriginScope arena_origin{speculative ? Shader::SrtArenaTrace::Origin::Speculative : Shader::SrtArenaTrace::Origin::Required};
#endif
    static const bool reported=[] { LOG_INFO(Render_Vulkan, "CPU/compiler diagnostic batch mask {} (1 priority, 2 retry, 4 library futures, 8 descriptor hash)", CompileCpuBatchMask()); return true; }();
    (void)reported;
    auto& jobs = translate_jobs[params.hash];
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(jobs, [&](const auto& job) {
        if (job->done && job->cancelled) return true;
        if (!job->done || job->module) return false;
        const bool same_inputs = std::ranges::equal(job->user_data, params.user_data);
        return !CompileCpuBatchEnabled(2) || !DelayFailedTranslationRetry(job->retry_after, now, speculative, same_inputs);
    });
    for (const auto& job : jobs) {
        if (MatchesTranslationRequest(*job, hw_stage, sw_stage, runtime_info, start)) {
#ifndef NDEBUG
            ++translate_stats.duplicate;
#endif
            return false;
        }
    }
    // Bound retained frontend pools as well as worker work. A required draw can
    // fall back to the existing synchronous path when the queue is full.
    size_t outstanding{};
    for (const auto& [queued_hash, queued_jobs] : translate_jobs) {
        for (const auto& queued : queued_jobs) outstanding += !queued->done;
    }
    if (outstanding >= 8) return false;
    auto job = std::make_shared<TranslateJob>(runtime_info);
    job->required = !speculative;
    job->hw_stage = hw_stage;
    job->sw_stage = sw_stage;
    job->hash = params.hash;
    std::ranges::copy(params.user_data, job->user_data.begin());
    job->code_base = params.Base();
    // FIX-160: the worker translates its own copy of the code. The game can release that memory
    // before a worker gets to it, and the read then crashed the worker (under Proton). Translation
    // does not use the code's address (pgm_base is set again when the result is taken).
    job->code_copy.resize(params.code.size());
#ifdef RYTY_GPU
    if (!Core::RytyMemory::GuardedCopy(job->code_copy.data(), params.code.data(),
                                       params.code.size_bytes())) {
        return false; // gone already; translated on the command thread if a draw needs it
    }
#else
    std::ranges::copy(params.code, job->code_copy.begin());
#endif
    job->code = job->code_copy;
    // Capture the separately allocated fetch code before the guest can recycle it.
    // Inline-fetch debugging accepts more instructions than the bounded fetch parser;
    // keep that optional mode on its existing synchronous path.
    if (EmulatorSettings.IsInlineFetchShader()) return false;
    try {
        Common::RecoverableScope recoverable;
        const auto read = [](uintptr_t address, u32& word) {
#ifdef RYTY_GPU
            return Core::RytyMemory::GuardedCopy(&word, reinterpret_cast<const void*>(address),
                                                sizeof(word));
#else
            std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
            return true;
#endif
        };
        const auto length = [](u32 word) -> u32 {
            using Shader::Gcn::InstEncoding;
            switch (Shader::Gcn::GetInstructionEncoding(word)) {
            case InstEncoding::MUBUF:
            case InstEncoding::MTBUF: return 2;
            case InstEncoding::SOP1:
            case InstEncoding::SMRD:
            case InstEncoding::VOP2:
            case InstEncoding::SOPP:
            case InstEncoding::VOP1: return 1;
            default: return 0;
            }
        };
        if (!Common::SnapshotFetchPrograms(job->code, job->user_data, read, length,
                                           job->fetch_code)) return false;
    } catch (const Common::RecoverableFailure&) {
        return false; // A real draw can retry; no worker may read the rejected pointer.
    }
    job->start = start;
    try {
        Common::RecoverableScope recoverable;
        const Shader::ShaderParams owned_params{
            .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{job->user_data},
            .code = job->code, .hash = job->hash,
        };
        job->info = std::make_unique<Shader::Info>(hw_stage, sw_stage, owned_params);
        // Pin the draw's vertex descriptors before translation and worker emission.
        const char* pin_fetch = std::getenv("RYGT_PIN_FETCH_SHARPS");
        job->info->pin_fetch_sharps = !pin_fetch || pin_fetch[0] != '0';
        job->info->pgm_base = job->code_base;
        auto prepared = std::make_shared<OwnedTranslation>(runtime_info);
        const auto* previous_fetch = Shader::t_fetch_code_snapshots;
        SCOPE_EXIT { Shader::t_fetch_code_snapshots = previous_fetch; };
        Shader::t_fetch_code_snapshots = &job->fetch_code;
#ifndef NDEBUG
        const auto preparation_start = std::chrono::steady_clock::now();
#endif
        prepared->program.emplace(Shader::TranslateProgram(job->code, prepared->pools, *job->info,
                                                           prepared->compiled_runtime, profile));
        job->retry_direct_only = DirectOnlyRetryInputs(
            job->info->srt_info.walker_func != nullptr, job->info->has_fetch_shader,
            bool(job->info->translated_fetch_shader)) && job->fetch_code.empty();
#ifndef NDEBUG
        job->translate_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - preparation_start).count();
#endif
        job->prepared_translation = std::move(prepared);
    } catch (const Common::RecoverableFailure&) {
        return false;
    } catch (const std::bad_alloc&) {
        return false;
    }
    jobs.push_back(job);
    try {
        translate_workers->Queue(job);
    } catch (const std::bad_alloc&) {
        jobs.pop_back(); // No worker owns it and no waiter can observe it under the lookup lock.
        return false;
    }
#ifndef NDEBUG
    ++translate_stats.queued;
#endif
    return true;
}

bool PipelineCache::TakeReadyTranslation(HwStage hw_stage, SwStage sw_stage, u64 hash,
                                         Program& program,
                                         const Shader::ShaderParams& params,
                                         const Shader::RuntimeInfo& runtime_info,
                                         const Shader::Backend::Bindings& binding, bool insert) {
    const auto it = ready_translations.find(hash);
    if (it == ready_translations.end()) {
        return false;
    }
    auto& ready = it.value();
    for (auto job_it = ready.begin(); job_it != ready.end(); ++job_it) {
        auto& job = *job_it;
        // Matched the way GetProgram matches a permutation: its specialization for this lookup.
        auto& info = *job->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        { Common::PhaseTimer flat_timer{Common::Phase::SelFlatten}; info.RefreshFlatBuf(); }
        // FIX-GT7-024: the lookup's on the right, as GetProgram compares (its bitsets decide).
        if (job->hw_stage != hw_stage || job->sw_stage != sw_stage ||
            job->spec != Shader::StageSpecialization(info, runtime_info, profile, binding)) {
            continue;
        }
        if (!insert) {
            return true;
        }
        size_t perm_idx = program.modules.size();
        if (const auto end = stored_perm_end.find(hash); end != stored_perm_end.end()) {
            perm_idx = std::max(perm_idx, end->second);
        }
        perm_idx = UnoccupiedShaderSlot(hash, perm_idx);
        const u64 perm_hash = HashCombine(hash, perm_idx);
        RegisterShaderMeta(info, job->spec.fetch_shader_data, job->spec, perm_hash, perm_idx);
        const u64 spv_hash = XXH3_64bits(job->spv.data(), job->spv.size() * sizeof(u32));
        Vulkan::SetObjectName(instance.GetDevice(), job->module,
                              GetShaderName(job->hw_stage, hash, perm_idx));
        program.InsertPermut(job->module, std::move(job->spec), std::move(job->info), perm_idx);
        program.modules[perm_idx].spv_hash = spv_hash;
        RegisterShaderBinary(std::move(job->spv), hash, perm_idx);
        ++Common::GetWorkCounters().shaders_compiled;
#ifndef NDEBUG
        ++translate_stats.used;
#endif
        ready.erase(job_it);
        if (ready.empty()) {
            ready_translations.erase(it);
        }
        return true;
    }
    return false;
}

void PipelineCache::NoteRequiredTranslations(u64 hash, HwStage hw_stage, SwStage sw_stage,
                                             const Shader::RuntimeInfo& runtime_info,
                                             const Shader::Backend::Bindings& binding) {
    const auto it = translate_jobs.find(hash);
    if (it == translate_jobs.end()) return;
    for (const auto& job : it->second) {
        if (!job->done.load() &&
            MatchesTranslationRequest(*job, hw_stage, sw_stage, runtime_info, binding) &&
            std::ranges::find(unlocked_translations, job) == unlocked_translations.end()) {
            translate_workers->Prioritize(job);
            unlocked_translations.push_back(job);
        }
    }
}

void PipelineCache::HarvestTranslations(u64 hash, bool wait, HwStage hw_stage, SwStage sw_stage,
                                       const Shader::RuntimeInfo& runtime_info,
                                       const Shader::Backend::Bindings& binding) {
    const auto it = translate_jobs.find(hash);
    if (it == translate_jobs.end()) {
        return;
    }
    auto& jobs = it.value();
    if (wait && t_required_lookup_lock && UnlockedShadersEnabled() && !regs_override &&
        !force_legacy && !fallback_nested && PreservesDepthOutput()) {
        NoteRequiredTranslations(hash, hw_stage, sw_stage, runtime_info, binding);
        wait = false;
    }
    if (wait) {
#ifndef NDEBUG
        const auto wait_start = std::chrono::steady_clock::now();
        bool waited = false;
#endif
        for (const auto& job : jobs) {
            if (MatchesTranslationRequest(*job, hw_stage, sw_stage, runtime_info, binding) &&
                !job->done) {
                // As CompileModule: the game never sees a PS4 wait for a shader.
                Common::GuestClock::CompilePause hold;
                RYDIAG_SCOPE(ShaderCompile, hash);
                // The draw needs this shader now; don't wait behind speculative work.
                if (CompileCpuBatchEnabled(1)) translate_workers->Prioritize(job);
                std::unique_lock lk{job->mutex};
                job->cv.wait(lk, [&] { return job->done.load(); });
#ifndef NDEBUG
                waited = true;
#endif
            }
        }
#ifndef NDEBUG
        if (waited) {
#ifndef NDEBUG
            ++translate_stats.waited;
#endif
            translate_stats.wait_ms += std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - wait_start)
                                           .count();
        }
#endif
    }
    auto& ready = ready_translations[hash];
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(jobs, [&](const std::shared_ptr<TranslateJob>& job) {
        if (!job->done) {
            return false;
        }
        if (job->cancelled) return true;
        if (!job->info || !job->module) {
            if (!job->failure_reported) {
#ifndef NDEBUG
                ++translate_stats.failed;
#endif
                job->failure_reported = true;
            }
            // Keep a brief negative result only for optional draws. Required
            // dependencies still retry/fall back, and changed user data bypasses it.
            return !CompileCpuBatchEnabled(2) || !DelayFailedTranslationRetry(job->retry_after, now, !wait, true);
        }
#ifndef NDEBUG
        translate_stats.translate_ms += job->translate_ms;
#endif
        // Tessellation keys were captured on the translating worker, not from
        // whatever the guest wrote to the constant buffer before harvesting.
        // Every stage key was captured from the owned compilation inputs on the worker.
        job->info->pinned_fetch_sharps.clear();
        job->info->pin_fetch_sharps = false;
        job->ready_since = now;
        ready.push_back(job);
        return true;
    });
    if (jobs.empty()) {
        translate_jobs.erase(it);
    }
    // Translations no draw took within 30 s came from states that were not drawn.
    std::erase_if(ready, [&](const std::shared_ptr<TranslateJob>& job) {
        if (now - job->ready_since < std::chrono::seconds{30} && ready.size() <= 32) {
            return false;
        }
        ForgetModuleCode(instance.GetDevice(), job->module);
        instance.GetDevice().destroyShaderModule(job->module);
#ifndef NDEBUG
        ++translate_stats.expired;
#endif
        return true;
    });
#ifndef NDEBUG
    auto& stats = translate_stats;
    if (now - stats.since >= std::chrono::seconds{2}) {
        if (stats.queued + stats.used != 0) {
            LOG_WARNING(Render_Vulkan,
                        "FIX-078 shader translation in {:.1f} s: {} queued, {} used by draws "
                        "({:.1f} ms of translation done off this thread), {} never drawn, {} "
                        "duplicates, {} failed; {} draws waited for one ({:.1f} ms)",
                        std::chrono::duration<double>(now - stats.since).count(), stats.queued,
                        stats.used, stats.translate_ms, stats.expired, stats.duplicate,
                        stats.failed, stats.waited, stats.wait_ms);
        }
        stats = {};
        stats.since = now;
    }
#endif
}

std::optional<PipelineCache::Result> PipelineCache::TryLegacyProgram(
    HwStage hw_stage, const Shader::ShaderParams& params, const Shader::RuntimeInfo& runtime_info,
    Shader::Backend::Bindings& binding) {
    auto& store = Storage::DataBase::Legacy();
    auto [it, is_new] = legacy_programs.try_emplace(params.hash);
    if (is_new) {
        it.value() = std::make_unique<Program>();
        // Like ApplyStoredListing, without its bookkeeping of this store's indices.
        auto listing = ReadStoredListing(params.hash, store);
        for (auto& entry : listing.entries) {
            if (!entry.has_spv) {
                continue;
            }
            auto info = std::make_unique<Shader::Info>();
            Shader::StageSpecialization spec{};
            spec.info = info.get();
            size_t stored_idx{};
            Serialization::Archive ar{std::move(entry.meta)};
            bool valid = false;
            try {
                Common::RecoverableScope recoverable;
                valid = ar.SizeBytes() >= 24 && LoadShaderMeta(ar, *info, spec, stored_idx);
            } catch (const Common::RecoverableFailure&) {
                valid = false;
            }
            if (!valid || stored_idx != entry.perm_idx || info->pgm_hash != params.hash ||
                std::ranges::any_of(info->buffers, [](const auto& buffer) {
                    return buffer.buffer_type == Shader::BufferType::DirectTable;
                })) {
                continue;
            }
            info->uses_direct = false; // the byte was padding in old metas
            it.value()->InsertPermut({}, std::move(spec), std::move(info), entry.perm_idx);
            it.value()->modules[entry.perm_idx].stored = true;
        }
#ifndef NDEBUG
        ++legacy_stats.programs;
#endif
    }
    auto& program = *it.value();
    for (size_t perm_idx = 0; perm_idx < program.modules.size(); ++perm_idx) {
        auto& permutation = program.modules[perm_idx];
        if (!permutation.info) {
            continue;
        }
        auto& info = *permutation.info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        const auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);
        if (permutation.spec != spec) {
            static std::atomic<u32> logged{};
            if (logged++ < 40) {
                u32 index{};
                const char* reason = permutation.spec.FirstDifference(spec, index);
                LOG_WARNING(Render_Vulkan, "D10: old translation {} of {}_{:#x} differs: {}[{}]",
                            perm_idx, hw_stage, params.hash, reason ? reason : "none", index);
            }
            continue;
        }
        if (!info.attribute_flags_known && IsTessEmulatedDraw()) {
            continue; // FIX-012 would store it again: left to the new translation
        }
        if (permutation.stored) {
            permutation.stored = false;
            permutation.module = LoadStoredModule(params.hash, perm_idx, store);
            if (!permutation.module) {
                permutation.info.reset();
                continue;
            }
        }
        info.AddBindings(binding);
        if (auto& fetch = permutation.spec.fetch_shader_data; !fetch.Empty()) {
            S().fetch_shader = &fetch;
        }
#ifndef NDEBUG
        ++legacy_stats.draws;
#endif
        ReportLegacy();
        // Its own pipeline keys: the module hashes of old translations differ from new ones.
        constexpr u64 LegacyTag = 0x4c45474143590000ULL;
        return std::make_tuple(&info, permutation.module,
                               HashCombine(params.hash, perm_idx) ^ LegacyTag);
    }
    return std::nullopt;
}

void PipelineCache::ReportLegacy() {
#ifndef NDEBUG
    const auto now = std::chrono::steady_clock::now();
    if (now - legacy_stats.since < std::chrono::seconds{2}) {
        return;
    }
    LOG_WARNING(Render_Vulkan,
                "D10: old-renderer shaders in 2 s: {} stage lookups used one, {} new translations "
                "queued for them, {} pipelines of old shaders, {} misses without one ({} programs "
                "read from the old store); pipelines not built yet: {} drawn with old shaders, "
                "{} old ones not built either, {} without old shaders",
                legacy_stats.draws, legacy_stats.queued, legacy_stats.pipelines,
                legacy_stats.no_legacy, legacy_stats.programs, legacy_stats.fallback_ok,
                legacy_stats.fallback_unbuilt, legacy_stats.fallback_none);
    const u64 programs = legacy_stats.programs;
    legacy_stats = {};
    legacy_stats.programs = programs;
    legacy_stats.since = now;
#endif
}

PipelineCache::Result PipelineCache::GetProgramForJob(HwStage hw_stage,
                                                      const Shader::ShaderParams& params,
                                                      const Shader::RuntimeInfo& runtime_info,
                                                      Shader::Backend::Bindings& binding) {
    // NAT-031: GetProgram for a game job thread (shared lock): only the programs and
    // permutations the threads above made are matched, in this thread's own copies of their
    // infos; nothing is created, loaded, harvested or remembered.
    const auto none = [] { return std::make_tuple(nullptr, vk::ShaderModule{}, u64{0}); };
    const auto it_pgm = program_cache.find(params.hash);
    if (it_pgm == program_cache.end() || !else_scope_checked.contains(params.hash)) {
        S().lookup_miss = "program not used yet";
        return none();
    }
    const Program& program = *it_pgm->second;
    bool tess_checked = false;
    // The permutation the last search found first (R7), then all of them in order.
    const size_t first = program.last_match;
    for (size_t step = 0; step <= program.modules.size(); ++step) {
        const size_t perm_idx = step == 0 ? first : step - 1;
        if (perm_idx >= program.modules.size() || (step != 0 && perm_idx == first)) {
            continue;
        }
        const auto& permutation = program.modules[perm_idx];
        if (!permutation.info) {
            continue;
        }
        auto& info = MatchInfo(*permutation.info);
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        if (hw_stage == HwStage::Hull && !tess_checked) {
            tess_checked = true; // FIX-058, as GetProgram
            if (!SaneTessConstants(info, liverpool, false)) {
                S().lookup_miss = "hull constants";
                return none();
            }
        }
        if (permutation.spec != Shader::StageSpecialization(info, runtime_info, profile, binding)) {
            continue;
        }
        if (permutation.stored) {
            S().lookup_miss = "stored translation not loaded";
            return none();
        }
        if (!info.attribute_flags_known && IsTessEmulatedDraw()) {
            S().lookup_miss = "rect/quad list attribute flags";
            return none();
        }
        info.AddBindings(binding);
        if (const auto& fetch = permutation.spec.fetch_shader_data; !fetch.Empty()) {
            S().fetch_shader = const_cast<Shader::Gcn::FetchShaderData*>(&fetch);
        }
        return std::make_tuple(&info, permutation.module, HashCombine(params.hash, perm_idx));
    }
    S().lookup_miss = hw_stage == HwStage::Fragment ? "no fragment translation for these inputs"
                      : hw_stage == HwStage::Vertex ? "no vertex translation for these inputs"
                                                     : "no translation for these inputs";
    return none();
}

// A cached permutation can describe a different fetch-shader layout from the current draw.
// Reject only that candidate; a later permutation or a fresh translation can still be valid.
static std::optional<Shader::StageSpecialization> TryMatchingSpecialization(
    const Shader::Info& info, const Shader::RuntimeInfo& runtime_info,
    const Shader::Profile& profile, Shader::Backend::Bindings binding) {
    try {
        return std::optional<Shader::StageSpecialization>{std::in_place, info, runtime_info,
                                                          profile, binding};
    } catch (const Common::RecoverableFailure& error) {
        static thread_local std::unordered_set<u64> reported;
        if (reported.insert(info.pgm_hash).second) {
            LOG_WARNING(Render_Vulkan,
                        "SHADER-CANDIDATE-REJECT: shader {:#x}: {}; trying other cached "
                        "permutations or a fresh translation",
                        info.pgm_hash, error.what());
        }
        return std::nullopt;
    }
}

namespace {
int SpecFastMode() {
    static const int mode = [] {
        const char* env = std::getenv("RYGT_SPEC_FAST");
        return env && *env ? std::clamp(env[0] - '0', 0, 2) : 1;
    }();
    return mode;
}
void RecordSpecInputs(Program::Module& module, const Shader::Info& info,
                      const Shader::RuntimeInfo& runtime, const Shader::Backend::Bindings& start) {
    if (!Shader::StageSpecialization::Packable(info)) return;
    thread_local Shader::Gcn::FetchShaderData fetch;
    module.inputs.words.clear();
    Shader::StageSpecialization::PackInputs(info, module.inputs.words, fetch);
    module.inputs.runtime_info = runtime;
    module.inputs.start = start;
    module.inputs.valid = true;
}
} // namespace

PipelineCache::Result PipelineCache::GetProgram(HwStage hw_stage, SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    // Cached shader matching also parses guest fetch shaders. Unlike CompileModule, that
    // path used to be outside a recovery scope: a transient invalid draw in the gift-car
    // scene stopped the entire game. Leave the shader unmatched and retry on a later draw.
    const auto original_binding = binding;
    try {
        Common::RecoverableScope recoverable;
        return GetProgramImpl(hw_stage, sw_stage, params, binding);
    } catch (const std::bad_alloc&) {
        binding = original_binding;
        S().fetch_shader = nullptr;
        ReportShaderAllocationFailure(hw_stage, params.hash, params.Base(), params.code.size(),
                                      "shader lookup or snapshot");
        return std::make_tuple(nullptr, vk::ShaderModule{}, u64{0});
    } catch (const Common::RecoverableFailure& error) {
        binding = original_binding;
        S().fetch_shader = nullptr;
        static thread_local std::unordered_map<u64, u64> failures;
        auto& count = failures[HashCombine(params.hash, u64(hw_stage))];
        ++count;
        if (count <= 3 || std::has_single_bit(count)) {
            LOG_WARNING(Render_Vulkan,
                        "SHADER-STATE-SKIP: {} shader {:#x} at {:#x}: {}; draw skipped, "
                        "state will be retried ({} occurrences)",
                        hw_stage, params.hash, params.Base(), error.what(), count);
        }
        return std::make_tuple(nullptr, vk::ShaderModule{}, u64{0});
    }
}

PipelineCache::Result PipelineCache::GetProgramImpl(HwStage hw_stage, SwStage sw_stage,
                                                    const Shader::ShaderParams& params,
                                                    Shader::Backend::Bindings& binding) {
    // SHD-002: a stage's bindings start where its own descriptor set does, whatever the stages
    // before it bound.
    binding = Shader::Backend::StageStart(u32(sw_stage));
    auto runtime_info = BuildRuntimeInfo(hw_stage, sw_stage);
    if (t_lookup_only) {
        return GetProgramForJob(hw_stage, params, runtime_info, binding); // NAT-031
    }
    if (program_cache.contains(params.hash)) pending_listings.erase(params.hash);
    if (store_readers && !program_cache.contains(params.hash)) {
        auto pending = pending_listings.find(params.hash);
        if (pending == pending_listings.end() && pending_listings.size() < 1024) {
            try { pending = pending_listings.emplace(params.hash, store_readers->List(params.hash).share()).first; }
            catch (const std::bad_alloc&) { /* Use the synchronous listing path. */ }
        }
        if (pending != pending_listings.end()) {
            const bool skip = SkipTranslationWait(sw_stage, params);
            const bool may_skip = regs_override != nullptr || defer_misses || skip;
            if (may_skip && pending->second.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
                if (!regs_override) { deferred_miss = true; translation_skipped |= skip; }
                else if (RetryBlockedReadAhead()) read_ahead_blocked = true;
                return std::make_tuple(nullptr, vk::ShaderModule{}, u64{0});
            }
            if (!may_skip) {
                if (t_required_lookup_lock && UnlockedShadersEnabled() &&
                    CanUnlockRequired(regs_override != nullptr, force_legacy, fallback_nested,
                                      defer_misses) && PreservesDepthOutput() &&
                    pending->second.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
                    // The map entry can be erased or rehashed while unlocked. Own its
                    // shared state, then let the outer lookup restart every stage/key.
                    unlocked_listings.push_back(pending->second);
                    deferred_miss = true;
                    return std::make_tuple(nullptr, vk::ShaderModule{}, u64{0});
                }
                pending->second.wait();
            }
        }
    }
    auto [it_pgm, new_program] = program_cache.try_emplace(params.hash);
    if (new_program) {
        it_pgm.value() = std::make_unique<Program>();
        if (use_stored_shaders) {
            const auto pending = pending_listings.find(params.hash);
            if (pending == pending_listings.end()) ListStoredPermutations(params.hash, *it_pgm.value());
            else {
                try { ApplyStoredListing(StoredListing{pending->second.get()}, *it_pgm.value()); }
                catch (...) { ListStoredPermutations(params.hash, *it_pgm.value()); }
                pending_listings.erase(pending);
            }
        }
    }
    // FIX-018: stored permutations of a program with an else after an empty if were translated
    // with the else running for every invocation. Translate those programs again, once; the new
    // permutations start after the stored ones, so their pipelines are new as well.
    const bool first_use = else_scope_checked.insert(params.hash).second;
#ifndef NDEBUG
    if (first_use && IsWatchedShader(params.hash)) {
        // DIAG-031: the guest code of watched shaders, raw and as a listing, in the log folder.
        const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
        const auto name = fmt::format("gcn_{:#x}", params.hash);
        Common::FS::IOFile{dir / (name + ".bin"), Common::FS::FileAccessMode::Create}.WriteSpan(
            params.code);
        std::ofstream{dir / (name + ".txt")} << Shader::ListGcnCode(params.code);
        LOG_WARNING(Render_Vulkan, "DIAG-031: wrote the code of {}_{:#x} ({} dwords) to {}",
                    hw_stage, params.hash, params.code.size(), (dir / (name + ".txt")).string());
    }
#endif
    // FIX-018/FIX-020: stored translations made before a translator fix that changes them.
    u32 needed_fixes = 0;
    bool legacy_lane_sensitive = false;
    // One program can be used with different compute workgroup sizes. Re-evaluate the cheap
    // policy from its cached code classification for each compute lookup, so an initial
    // <=32-thread use cannot hide a later 64-thread translation needing the fix.
    if (first_use || force_legacy || hw_stage == HwStage::Compute) {
      const auto stored_end = stored_perm_end.find(params.hash);
      const size_t stored_boundary = stored_end == stored_perm_end.end() ? 0 : stored_end->second;
      if (const auto* scan = shader_fix_scans.GetIfNeeded(
            params.hash, first_use, force_legacy, hw_stage == HwStage::Compute, stored_boundary, [&] {
            u32 flags = Shader::HasEmptyScopeBeforeElse(params.code) ? 1u : 0u;
            if (Shader::HasSdwaCompareDestination(params.code)) flags |= 32u;
            if (Shader::HasLegacyMad(params.code)) flags |= 16u;
            return flags;
        }, [&] {
            return std::pair{Shader::HasLaneReads(params.code),
                             Shader::HasLaneReadsAndLoop(params.code)};
        })) {
        needed_fixes |= scan->base_fixes;
        const auto& wg = runtime_info.hw.cs.workgroup_size;
        if (hw_stage == HwStage::Compute && profile.subgroup_size < 64 &&
            wg[0] * wg[1] * wg[2] > 32 && Common::PerfFeatureEnabled(31) &&
            scan->lane_reads_in_loop) {
            needed_fixes |= 8u | 128u; // Wave64 lowering also changes stored loop translations.
        }
        if (hw_stage == HwStage::Compute && profile.subgroup_size < 64 &&
            wg[0] * wg[1] * wg[2] == 64 && Common::PerfFeatureEnabled(31)) {
            needed_fixes |= 128u;
        }
        // FIX-GT7-028 has its own policy and does not depend on the loop-optimization switch.
        // Bit 1024 was also written with unbranching disabled; do not trust it as proof.
        if (hw_stage == HwStage::Compute && profile.subgroup_size < 64 &&
            u64(wg[0]) * wg[1] * wg[2] == 64 &&
            (force_legacy || Shader::LaneReadUnbranchEnabled()) && scan->lane_reads) {
            legacy_lane_sensitive = force_legacy;
            needed_fixes |= Shader::LaneReadFixMarker(true, profile.subgroup_size, 64,
                                                      Shader::LaneReadUnbranchEnabled());
        }
      }
    }
    const auto known = else_scope_fixed.find(params.hash);
    const auto session = session_fix_marks.find(params.hash);
    const u32 applied_fixes = (known != else_scope_fixed.end() ? known->second.fixes : 0u) |
                              (session != session_fix_marks.end() ? session->second : 0u);
    if ((needed_fixes & ~applied_fixes) != 0) {
        auto& boundary = stored_perm_end[params.hash];
        boundary = std::max(boundary, it_pgm.value()->modules.size());
        if (boundary != 0) {
            LOG_WARNING(Render_Vulkan,
                        "FIX-018/020: translating {}_{:#x} again (fixes {:#x}, {} stored "
                        "permutations)",
                        hw_stage, params.hash, needed_fixes, boundary);
            if (!it_pgm.value()->modules.empty()) {
                // Pipelines already built keep the old modules alive.
                retired_programs.push_back(std::move(it_pgm.value()));
                it_pgm.value() = std::make_unique<Program>();
            }
        }
        if (boundary != 0) {
            else_scope_fixed[params.hash] = {boundary, applied_fixes | needed_fixes};
            SaveElseScopeFixed();
        } else {
            // No stored permutation was observed. Record this only for the current session:
            // another insertion path may have bypassed listing existing disk records.
            session_fix_marks[params.hash] = applied_fixes | needed_fixes;
        }
    }

    // D10's separate store has no trusted record of the current translator fixes. Reuse it only
    // when the guest code does not require one; preserve every legacy file for other uses. Keep
    // this after the normal-store marker update even when this is the program's first use.
    if (force_legacy && (needed_fixes != 0 || legacy_lane_sensitive)) {
        S().legacy_failed = true;
        return std::make_tuple(nullptr, vk::ShaderModule{}, u64{0});
    }
    auto& program = it_pgm.value();
    // FIX-078: translations made on the workers become permutations; a draw (not a read-ahead)
    // first waits for any still running, as one of them is likely the permutation it needs.
    // D10: with the fallback a draw does not wait for translations in progress (the old
    // translations draw meanwhile); only when there are none (fallback_nested).
    const bool skip_wait = SkipTranslationWait(sw_stage, params); // SHD-005
    HarvestTranslations(params.hash, regs_override == nullptr && !force_legacy && !skip_wait &&
                                         (!legacy_fallback || fallback_nested),
                        hw_stage, sw_stage, runtime_info, binding);
    // D10: a key is made of new translations only, or (forced) of old ones only.
    if (force_legacy) {
        if (auto legacy = TryLegacyProgram(hw_stage, params, runtime_info, binding)) {
            S().key_uses_legacy = true;
            return *legacy;
        }
        S().legacy_failed = true;
        return std::make_tuple(nullptr, vk::ShaderModule{}, u64{0});
    }
    std::optional<Common::PhaseTimer> match_timer{std::in_place, Common::Phase::ProgramMatch};
    // Diagnostic for runaway permutations: why each existing permutation was rejected.
    std::string mismatch_reasons;
    // PERF-044: replay the last search when its inputs are the same. -DisablePerf 56.
    static const bool memo_enabled = Common::PerfFeatureEnabled(56);
    static const bool own_ahead_match = [] {
        const char* value = std::getenv("RYGT_READ_AHEAD_OWN_MATCH");
        return !(value && value[0] == '0');
    }();
    const bool ahead = own_ahead_match && regs_override != nullptr;
    auto& memo = program->SearchMemo(ahead);
    auto& last_match = program->SearchLastMatch(ahead);
    const bool memo_stage = memo_enabled && (hw_stage == HwStage::Fragment ||
                                             hw_stage == HwStage::Compute);
    bool memo_usable = memo_stage && memo.valid &&
                       memo.modules_size == program->modules.size() &&
                       memo.runtime_info == runtime_info && memo.start == binding &&
                       std::ranges::equal(memo.user_data, params.user_data);
    const auto start_binding = binding;
    boost::container::small_vector<std::vector<u32>, 4> flats;
    // Leaves the memo's replay: the buffers of the permutations before `end` were the memo's.
    const auto stop_replay = [&](size_t end) {
        memo_usable = false;
        flats.clear();
        for (size_t i = 0; i < end; ++i) {
            flats.push_back(i < memo.flats.size() ? memo.flats[i] : std::vector<u32>{});
        }
    };
    // R7: the permutation the last search found is tried first; a program has up to dozens and
    // each one tried builds a specialization (the command thread's largest cost per draw).
    // -DisablePerf 133 searches from the first every time.
    static const bool last_first = Common::PerfFeatureEnabled(133);
    if (last_first && !memo_usable && hw_stage != HwStage::Hull &&
        last_match < program->modules.size()) {
        auto& permutation = program->modules[last_match];
        if (permutation.info && !permutation.stored) {
            auto& info = *permutation.info;
            info.pgm_base = params.Base();
            info.user_data = params.user_data;
            { Common::PhaseTimer flat_timer{Common::Phase::SelFlatten}; info.RefreshFlatBuf(); }
            const int fast_mode = SpecFastMode();
            bool packed_match = false;
            if (fast_mode && permutation.inputs.valid &&
                Shader::StageSpecialization::Packable(info) &&
                permutation.inputs.start == binding && permutation.inputs.runtime_info == runtime_info) {
                thread_local std::vector<u64> words;
                thread_local Shader::Gcn::FetchShaderData fetch;
                words.clear();
                Shader::StageSpecialization::PackInputs(info, words, fetch);
                packed_match = words == permutation.inputs.words &&
                               fetch == permutation.spec.fetch_shader_data;
            }
            bool matched = packed_match && fast_mode == 1;
            if (!matched) {
                Common::PhaseTimer spec_timer{Common::Phase::SelSpec};
                const auto current_spec = TryMatchingSpecialization(info, runtime_info, profile, binding);
                matched = current_spec && permutation.spec == *current_spec;
                if (packed_match && !matched) {
                    LOG_ERROR(Render_Vulkan, "PERF-GT7-058 packed specialization mismatch: {:#x}", params.hash);
                }
                if (matched && fast_mode) RecordSpecInputs(permutation, info, runtime_info, binding);
            }
            if (matched &&
                !(AsyncTranslation() && !info.attribute_flags_known && IsTessEmulatedDraw())) {
                if (!info.attribute_flags_known && IsTessEmulatedDraw()) {
                    RecoverAttributeFlags(info, permutation.spec, params, runtime_info,
                                          last_match);
                }
                info.AddBindings(binding);
                if (auto& fetch = permutation.spec.fetch_shader_data; !fetch.Empty()) {
                    S().fetch_shader = &fetch;
                }
                return std::make_tuple(&info, permutation.module,
                                       HashCombine(params.hash, last_match));
            }
        }
    }
    bool tess_checked = false;
    for (size_t perm_idx = 0; perm_idx < program->modules.size(); ++perm_idx) {
        auto& permutation = program->modules[perm_idx];
        if (!permutation.info) {
            if (memo_stage && !memo_usable) {
                flats.emplace_back();
            }
            continue;
        }
        auto& info = *permutation.info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        { Common::PhaseTimer flat_timer{Common::Phase::SelFlatten}; info.RefreshFlatBuf(); }
        if (hw_stage == HwStage::Hull && !tess_checked) {
            // FIX-058: checked with the first translation's info, before a new permutation
            // could be made from garbage constants.
            tess_checked = true;
            if (!SaneTessConstants(info, liverpool, regs_override != nullptr)) {
                return std::make_tuple(nullptr, vk::ShaderModule{}, u64{0});
            }
        }
        if (memo_usable && (perm_idx > memo.perm || memo.flats[perm_idx] != info.flattened_ud_buf)) {
            // The inputs differ from here on; the search goes on the usual way.
            stop_replay(perm_idx);
        }
        if (memo_stage && !memo_usable) {
            flats.push_back(info.flattened_ud_buf);
        }
        // Old cache metadata needs a fresh translation to recover attribute flags.
        // Let the existing worker miss path do it instead of recompiling here.
        if (AsyncTranslation() && !info.attribute_flags_known && IsTessEmulatedDraw()) {
            continue;
        }
        bool matches;
        if (memo_usable) {
            matches = perm_idx == memo.perm;
        } else {
            std::optional<Common::PhaseTimer> spec_timer{std::in_place, Common::Phase::SelSpec};
            const auto spec = TryMatchingSpecialization(info, runtime_info, profile, binding);
            spec_timer.reset();
            if (!spec) {
                continue;
            }
            matches = permutation.spec == *spec;
            if (!matches && program->modules.size() >= 4 && mismatch_reasons.size() < 600) {
                u32 index{};
                const char* reason = permutation.spec.FirstDifference(*spec, index);
                mismatch_reasons +=
                    fmt::format(" {}:{}[{}]", perm_idx, reason ? reason : "none", index);
            }
        }
        // Newly active resources must match the compiled specialization; inactive ones can reuse
        // it.
        if (!matches) {
            continue;
        }
        if (permutation.stored) {
            // PERF-032: a stored translation matched; its SPIR-V is read now.
            permutation.stored = false;
            permutation.module = LoadStoredModule(params.hash, perm_idx);
            if (!permutation.module) {
                permutation.info.reset();
                if (memo_usable) {
                    stop_replay(perm_idx + 1);
                }
                memo.valid = false;
                continue;
            }
        }
        if (memo_stage && !memo_usable) {
            memo.valid = true;
            memo.perm = perm_idx;
            memo.modules_size = program->modules.size();
            memo.runtime_info = runtime_info;
            memo.start = start_binding;
            memo.user_data.assign(params.user_data.begin(), params.user_data.end());
            memo.flats.assign(flats.begin(), flats.end());
        }
        last_match = perm_idx; // R7
        if (!memo_usable && SpecFastMode()) RecordSpecInputs(permutation, info, runtime_info, binding);
        if (!info.attribute_flags_known && IsTessEmulatedDraw()) {
            RecoverAttributeFlags(info, permutation.spec, params, runtime_info, perm_idx);
        }
        info.AddBindings(binding);
        if (auto& fetch = permutation.spec.fetch_shader_data; !fetch.Empty()) {
            S().fetch_shader = &fetch;
        }
        return std::make_tuple(&info, permutation.module, HashCombine(params.hash, perm_idx));
    }
    match_timer.reset();
    if (AsyncTranslation()) {
        // FIX-078: a translation the workers made for these inputs.
        const bool read_ahead = regs_override != nullptr;
        const bool insert = !read_ahead || RetryBlockedReadAhead();
        if (TakeReadyTranslation(hw_stage, sw_stage, params.hash, *program, params, runtime_info,
                                 binding, insert)) {
            if (!insert) {
                return std::make_tuple(nullptr, vk::ShaderModule{}, u64{0});
            }
            return GetProgram(hw_stage, sw_stage, params, binding);
        }
        // D10: the new translation is made on a worker; meanwhile the draw's key is made of the
        // old renderer's translations (GetGraphicsPipeline / GetComputePipeline). Without them,
        // the usual way (fallback_nested).
        if (legacy_fallback && !MenusWait() && !read_ahead && !fallback_nested) {
            if (QueueTranslation(hw_stage, sw_stage, params, runtime_info, binding, true)) {
#ifndef NDEBUG
                ++legacy_stats.queued;
#endif
            }
            fallback_miss = true;
            return std::make_tuple(nullptr, vk::ShaderModule{}, u64{0});
        }
        const bool unlocked = t_required_lookup_lock && UnlockedShadersEnabled() && !read_ahead &&
                              !force_legacy && !fallback_nested && PreservesDepthOutput();
        if (read_ahead || defer_misses || skip_wait || unlocked) {
            // A shader an upcoming draw needs (or this draw, which looks again once the next
            // draws' shaders are queued too); translated on a worker while this thread reads
            // on. No key without it: the state is skipped for now. SHD-005: a stage's bindings
            // no longer follow from the stages before it, so a race queues every missing stage
            // of the draw at once and skips it.
            if (!S().stage_deferred || skip_wait || unlocked ||
                (read_ahead && RetryBlockedReadAhead())) {
                S().stage_deferred = true;
                QueueTranslation(hw_stage, sw_stage, params, runtime_info, binding, !unlocked);
            }
            if (unlocked) NoteRequiredTranslations(params.hash, hw_stage, sw_stage, runtime_info, binding);
            if (!read_ahead) {
                deferred_miss = true;
                translation_skipped |= skip_wait;
            } else if (RetryBlockedReadAhead()) {
                read_ahead_blocked = true;
            }
            return std::make_tuple(nullptr, vk::ShaderModule{}, u64{0});
        }
        // A required command (compute, depth bake or other dependency) cannot be
        // dropped, but its translation can still execute on the worker. Retry
        // once if a prior job used different guest resource specializations.
        for (u32 attempt = 0; attempt < 2; ++attempt) {
            QueueTranslation(hw_stage, sw_stage, params, runtime_info, binding);
            HarvestTranslations(params.hash, true, hw_stage, sw_stage, runtime_info, binding);
            if (TakeReadyTranslation(hw_stage, sw_stage, params.hash, *program, params,
                                     runtime_info, binding, true)) {
                return GetProgram(hw_stage, sw_stage, params, binding);
            }
        }
        // Retain the required-draw fallback below if worker snapshot/translation
        // failed. In particular, never silently omit a one-time shadow bake.
    }

    // PERF-013: runtime permutations used to take the next free slot of this session, which
    // depends on which stored permutations were preloaded. A slot already used in the stored
    // cache then got a different specialization, overwriting the stored shader and leaving
    // every pipeline that used it unloadable ("stale"; about 1,085 per GT Sport run, compiled
    // again in every race). Start after the stored indices instead.
    size_t perm_idx = program->modules.size();
    if (const auto it = stored_perm_end.find(params.hash); it != stored_perm_end.end()) {
        perm_idx = std::max(perm_idx, it->second);
    }
    perm_idx = UnoccupiedShaderSlot(params.hash, perm_idx);
    const u64 perm_hash = HashCombine(params.hash, perm_idx);
    if (!mismatch_reasons.empty()) {
        static u64 runaway_events = 0;
        if (++runaway_events <= 40 || runaway_events % 500 == 0) {
            LOG_WARNING(Render_Vulkan, "New permutation {} of {}_{:#x} (event {}); rejected:{}",
                        perm_idx, hw_stage, params.hash, runaway_events, mismatch_reasons);
        }
    }
    const auto start = binding;
    auto info = std::make_unique<Shader::Info>(hw_stage, sw_stage, params);
    // Compilation passes may adjust their runtime info (clip-distance emulation adds a fragment
    // input). The stored key must use the unmodified runtime info, or it never matches the next
    // lookup and the same module is recompiled for every draw.
    auto compile_runtime_info = runtime_info;
    const auto module = CompileModule(*info, compile_runtime_info, params.code, perm_idx, binding);
    if (!module) {
        // A failed allocation/translation must not install a permanently unmatched null module.
        binding = start;
        return std::make_tuple(nullptr, vk::ShaderModule{}, u64{0});
    }
    auto spec = Shader::StageSpecialization(*info, runtime_info, profile, start);
    KeepCompiledTessellationLayout(spec, compile_runtime_info);
    RegisterShaderMeta(*info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
    const auto* info_ptr = info.get();
    program->InsertPermut(module, std::move(spec), std::move(info), perm_idx);
    program->modules[perm_idx].spv_hash = last_spv_hash;
    LogRepeatedPermutation(*program, perm_idx, hw_stage, params.hash);
    if (auto& fetch = program->modules[perm_idx].spec.fetch_shader_data; !fetch.Empty()) {
        S().fetch_shader = &fetch;
    }
    return std::make_tuple(info_ptr, module, perm_hash);
}

void PipelineCache::LogRepeatedPermutation(const Program& program, size_t perm_idx,
                                           Shader::HwStage hw_stage, u64 pgm_hash) {
    // FIX-043 diagnostic: a new permutation whose SPIR-V equals an existing one's was translated
    // only because their keys differ in a property its code does not read.
    const auto& added = program.modules[perm_idx];
    if (added.spv_hash == 0) {
        return;
    }
    for (size_t i = 0; i < program.modules.size(); ++i) {
        const auto& other = program.modules[i];
        if (i == perm_idx || !other.info || other.spv_hash != added.spv_hash) {
            continue;
        }
        static u64 repeats = 0;
        ++repeats;
        if (repeats <= 40 || repeats % 200 == 0) {
            u32 index{};
            const char* reason = other.spec.FirstDifference(added.spec, index);
            LOG_WARNING(Render_Vulkan,
                        "FIX-043: permutation {} of {}_{:#x} has the same SPIR-V as permutation "
                        "{} (keys differ first in {}[{}]); {} such repeats so far",
                        perm_idx, hw_stage, pgm_hash, i, reason ? reason : "none", index, repeats);
        }
        return;
    }
}

bool PipelineCache::IsTessEmulatedDraw() const {
    const auto prim = Regs().primitive_type;
    return prim == AmdGpu::PrimitiveType::RectList ||
           (prim == AmdGpu::PrimitiveType::QuadList && !QuadListAsTriangles());
}

bool PipelineCache::QuadListAsTriangles() const {
    // PERF-028: the rasterizer draws quad lists as triangle lists through generated indices
    // (Rasterizer::BindQuadListIndices). Their pipeline is a plain triangle-list pipeline, and
    // the vertex shader writes the fragment shader's inputs directly.
    return Regs().primitive_type == AmdGpu::PrimitiveType::QuadList &&
           !S().draw_indirect_params.tessellate_quads && Common::PerfFeatureEnabled(28);
}

void PipelineCache::RecoverAttributeFlags(Shader::Info& info,
                                          const Shader::StageSpecialization& spec,
                                          const Shader::ShaderParams& params,
                                          const Shader::RuntimeInfo& runtime_info,
                                          size_t perm_idx) {
#ifndef NDEBUG
    Shader::SrtArenaTrace::OriginScope arena_origin{Shader::SrtArenaTrace::Origin::Flags};
#endif
    // FIX-012: a shader from a cache entry stored without its attribute loads/stores is
    // translated again for them (its stored SPIR-V is kept), because the rect/quad-list helper
    // shaders forward exactly the outputs these flags name. The entry is stored again with them.
    Common::GuestClock::CompilePause hold; // FIX-044
    Shader::Info translated{info.hw_stage, info.sw_stage, params};
    auto translate_runtime_info = runtime_info;
    [[maybe_unused]] const auto program =
        Shader::TranslateProgram(params.code, pools, translated, translate_runtime_info, profile);
    info.loads = translated.loads;
    info.stores = translated.stores;
    info.attribute_flags_known = true;
    RegisterShaderMeta(info, spec.fetch_shader_data, spec, HashCombine(params.hash, perm_idx),
                       perm_idx);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    std::unique_lock lock{lookup_mutex};
    if (!CanReplaceShader(unlocked_required_waits,
                          !pending_builds.empty() || (build_workers && build_workers->Busy()))) {
        LOG_WARNING(Render_Vulkan, "Shader replacement deferred while pipeline builds are active");
        return std::nullopt;
    }
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                ForgetModuleCode(d, m.module);
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d, instance.IsShaderObjectEnabled());
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
#ifndef NDEBUG
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
#else
    (void)code; (void)hash; (void)stage; (void)perm_idx; (void)ext;
#endif
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan


namespace Vulkan {
void PipelineCache::ReleaseCompletedPipelineOwners(std::vector<std::shared_ptr<const void>> owners) {
    PipelineRetirementWorker().Enqueue([owners = std::move(owners)] {});
}
std::shared_ptr<const void> PipelineCache::KeepAlive(const GraphicsPipeline* pipeline) const {
    if (!pipeline) return {};
    const auto it = graphics_pipelines.find(pipeline->GetGraphicsKey());
    ASSERT(it != graphics_pipelines.end() && it->second.get() == pipeline);
    return it->second;
}
std::shared_ptr<const void> PipelineCache::KeepAlive(const ComputePipeline* pipeline) const {
    if (!pipeline) return {};
    const auto it = compute_pipelines.find(compute_key);
    ASSERT(it != compute_pipelines.end() && it->second.get() == pipeline);
    return it->second;
}
void PipelineCache::TrimUnusedPipelines() {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (now - retention_last_scan < milliseconds{250}) return;
    retention_last_scan = now;
    // Retain reusable pipelines until the live device-local budget needs headroom.
    // VMA allocation totals omit driver-owned pipeline memory; query the driver.
    // Without budget reporting we cannot establish pressure, so retain pipelines.
    if (!instance.CanReportMemoryUsage()) return;
    vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    vk::PhysicalDeviceMemoryProperties2 memory{.pNext = &budget};
#ifndef NDEBUG
    const auto budget_query_start = steady_clock::now();
#endif
    instance.GetPhysicalDevice().getMemoryProperties2(&memory);
#ifndef NDEBUG
    const auto budget_query_end = steady_clock::now();
    if (budget_query_end - budget_query_start >= milliseconds{1}) {
        LOG_WARNING(Render_Vulkan, "BUDGET-QUERY: pipeline retention {:.3f} ms",
                    duration<double, std::milli>(budget_query_end - budget_query_start).count());
    }
#endif
    u64 usage_bytes = 0;
    u64 budget_bytes = 0;
    for (u32 i = 0; i < memory.memoryProperties.memoryHeapCount; ++i) {
        const auto& heap = memory.memoryProperties.memoryHeaps[i];
        if (!(heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal)) continue;
        usage_bytes += budget.heapUsage[i];
        budget_bytes += std::min<u64>(budget.heapBudget[i], heap.size);
    }
    const bool under_pressure = PipelineRetentionUnderPressure(usage_bytes, budget_bytes);
    std::unique_lock lock{lookup_mutex, std::try_to_lock};
    if (!lock.owns_lock()) return;
    retention_now = now;
    if (!under_pressure) return;
    if (PipelineRetirementWorker().PendingCount() >= 512) return;
    // Queued selections hold a shared reference; retirement cannot invalidate them.
    constexpr size_t retire_batch = 128;
    const auto cutoff = now - seconds{5};
    std::vector<std::shared_ptr<const void>> retired;
    const auto trim = [&](auto& cache) {
        using Entry = typename std::remove_reference_t<decltype(cache)>::value_type;
        using Key = std::remove_const_t<typename Entry::first_type>;
        std::vector<std::pair<steady_clock::time_point, Key>> candidates;
        for (const auto& [key, pipeline] : cache) {
            if (!pipeline || pipeline.use_count() != 1) continue;
            if constexpr (std::is_same_v<Key, GraphicsPipelineKey>) {
                if (!pipeline->CanRetire()) continue;
            }
            const auto found = pipeline_last_use.find(pipeline.get());
            if (found != pipeline_last_use.end()) {
                const auto last_use = std::max(found->second, pipeline->LastRetentionUse());
                if (last_use < cutoff) candidates.emplace_back(last_use, key);
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [used, key] : candidates) {
            if (retired.size() >= retire_batch) break;
            auto it = cache.find(key);
            pipeline_last_use.erase(it->second.get());
            retired.emplace_back(std::move(it.value()));
            cache.erase(it);
        }
    };
    trim(graphics_pipelines);
    trim(compute_pipelines);
    if (retired.empty()) return;
    const auto count = retired.size();
    // Includes commands already recorded in the current, not-yet-submitted batch.
    // GPU completion on the scheduler only delivers ownership to a separate worker.
    // Neither recorder nor readback worker executes pipeline destructors.
    scheduler.DeferOperation([retired = std::move(retired)]() mutable {
        for (auto& pipeline : retired) {
            PipelineRetirementWorker().Enqueue([pipeline = std::move(pipeline)] {});
        }
    });
    LOG_INFO(Render_Vulkan, "Pipeline memory pressure: {} MiB used / {} MiB budget, "
             "2048 MiB reserve; retired {} unused pipelines, {} cached",
             usage_bytes >> 20, budget_bytes >> 20, count,
             graphics_pipelines.size() + compute_pipelines.size());
}
}
