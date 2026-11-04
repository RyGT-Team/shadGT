// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "common/guest_clock.h"
#include "ryty_gpu/ryty_framegen.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "ryty_gpu/ryty_fsr3.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"

#include "ryty_gpu/ffx_api/ffx_api.h"
#include "ryty_gpu/ffx_api/ffx_framegeneration.h"
#include "ryty_gpu/ffx_api/vk/ffx_api_vk.h"

namespace Vulkan::RytyFrameGen {

namespace {

struct Api {
    HMODULE module{};
    PfnFfxCreateContext create_context{};
    PfnFfxDestroyContext destroy_context{};
    PfnFfxConfigure configure{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};
    bool tried{};
};
Api g_api;

#ifndef NDEBUG
// Opt-in, fixed-size trace. Never reuse queries or wait for GPU results in the frame loop.
struct TimingTrace {
    static constexpr u32 Count = 16;
    vk::Device device{};
    vk::QueryPool pool{};
    double period{};
    u64 mask{};
    u32 used{};
    std::array<std::atomic<u64>, Count> ids{};
    std::array<std::atomic<bool>, Count> generated{};
    std::array<bool, Count> reported{};
    std::array<std::atomic<u64>, Count * 2> extra_ids{};
    std::array<bool, Count * 2> extra_reported{};
    std::array<u32, 2> extra_used{}; // recorder thread only
    struct CpuEvent { const char* kind{}; s64 begin{}, end{}; };
    std::array<CpuEvent, 128> cpu_events{};
    u32 cpu_used{}; // present thread only
    std::atomic<bool> cpu_armed{};

    static s64 Now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void Cpu(const char* kind, s64 begin) {
        if (!cpu_armed.load(std::memory_order_relaxed) || cpu_used == cpu_events.size()) return;
        cpu_events[cpu_used++] = {kind, begin, Now()};
        if (cpu_used == cpu_events.size()) {
            const auto origin = cpu_events[0].begin;
            for (const auto& event : cpu_events)
                LOG_INFO(Render_Vulkan, "FG host timing: {} at {:.3f} ms duration {:.3f} ms",
                    event.kind, (event.begin - origin) / 1e6, (event.end - event.begin) / 1e6);
        }
    }
    int BeginExtra(vk::CommandBuffer cmd, u64 sequence, u32 stage) {
        if (!pool || !cpu_armed.load(std::memory_order_relaxed) ||
            stage >= 2 || extra_used[stage] == Count) return -1;
        const u32 slot = stage * Count + extra_used[stage]++;
        // Sequence + 1 reserves zero for an unused slot.
        extra_ids[slot].store(sequence + 1, std::memory_order_release);
        const u32 query = Count * 4 + slot * 2;
        cmd.resetQueryPool(pool, query, 2);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
        return static_cast<int>(query);
    }

    void Create(const Instance& instance) {
        const char* option = std::getenv("RYGT_FG_GPU_TIMING");
        if (!option || std::string{option} != "1") return;
        const auto families = instance.GetPhysicalDevice().getQueueFamilyProperties();
        const auto bits = families[instance.GetGraphicsQueueFamilyIndex()].timestampValidBits;
        if (!bits) return;
        device = instance.GetDevice();
        period = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
        mask = bits == 64 ? ~0ULL : ((1ULL << bits) - 1);
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = Count * 8;
        VkQueryPool handle{};
        if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateQueryPool(device, &info, nullptr, &handle) == VK_SUCCESS) {
            pool = handle;
            LOG_INFO(Render_Vulkan, "FG GPU timing: armed for {} frames", Count);
        }
    }
    int Begin(vk::CommandBuffer cmd, u64 id, bool generation) {
        if (!pool) return -1;
        u32 slot{};
        if (generation) {
            for (; slot < Count; ++slot)
                if (ids[slot].load(std::memory_order_acquire) == id) break;
            if (slot == Count || generated[slot].exchange(true)) return -1;
        } else {
            if (used == Count) return -1;
            slot = used++;
            ids[slot].store(id, std::memory_order_release);
        }
        const u32 query = slot * 4 + (generation ? 2 : 0);
        cmd.resetQueryPool(pool, query, 2);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
        return static_cast<int>(query);
    }
    void End(vk::CommandBuffer cmd, int query) {
        if (query >= 0)
            cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query + 1);
    }
    void Poll(u64 current) {
        if (!pool) return;
        for (u32 slot = 0; slot < Count * 2; ++slot) {
            const u64 sequence = extra_ids[slot].load(std::memory_order_acquire);
            if (!sequence || extra_reported[slot]) continue;
            struct Result { u64 value, available; } values[2]{};
            const auto result = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueryPoolResults(
                device, pool, Count * 4 + slot * 2, 2, sizeof(values), values, sizeof(Result),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (result != VK_SUCCESS || !values[0].available || !values[1].available) continue;
            extra_reported[slot] = true;
            LOG_INFO(Render_Vulkan, "FG GPU timing: scene {} {} {:.3f} ms",
                sequence - 1, slot < Count ? "snapshot copies" : "HUD conversion",
                ((values[1].value - values[0].value) & mask) * period / 1e6);
        }
        for (u32 slot = 0; slot < used; ++slot) {
            const u64 id = ids[slot].load(std::memory_order_acquire);
            if (reported[slot] || current < id + 8 || !generated[slot].load()) continue;
            struct Result { u64 value, available; } values[4]{};
            const auto result = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueryPoolResults(
                device, pool, slot * 4, 4, sizeof(values), values, sizeof(Result),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (result != VK_SUCCESS) continue;
            if (!values[0].available || !values[1].available ||
                !values[2].available || !values[3].available) continue;
            reported[slot] = true;
            LOG_INFO(Render_Vulkan, "FG GPU timing: frame {} prepare {:.3f} ms interpolation {:.3f} ms",
                id, ((values[1].value - values[0].value) & mask) * period / 1e6,
                ((values[3].value - values[2].value) & mask) * period / 1e6);
        }
    }
    void Destroy() {
        // Called after the FG contexts stop their callbacks and the swapchain is idle.
        if (pool) device.destroyQueryPool(pool);
        pool = nullptr;
        used = 0;
        extra_used = {};
        cpu_used = 0;
        cpu_armed.store(false);
        for (u32 i = 0; i < Count * 2; ++i) {
            extra_ids[i].store(0);
            extra_reported[i] = false;
        }
        for (u32 i = 0; i < Count; ++i) {
            ids[i].store(0);
            generated[i].store(false);
            reported[i] = false;
        }
    }
} g_timing;
#else
struct TimingTrace {
    std::atomic<bool> cpu_armed{};
    static constexpr s64 Now() { return 0; }
    void Cpu(const char*, s64) {}
    int BeginExtra(vk::CommandBuffer, u64, u32) { return -1; }
    void Create(const Instance&) {}
    int Begin(vk::CommandBuffer, u64, bool) { return -1; }
    void End(vk::CommandBuffer, int) {}
    void Poll(u64) {}
    void Destroy() {}
} g_timing;
#endif

// Diagnostic workaround for the bundled SDK's 100 ms pacing-history timeout.
// This constant has one code reference: the interpolation-thread reset threshold.
// Check the entire DLL on disk and the loaded instructions before touching its data.
bool ApplyLowFpsPacing(HMODULE module, const std::wstring& path) {
    const char* option = std::getenv("RYGT_FG_LOW_FPS_PACING");
    if (!option || std::string{option} != "1") return false;
    constexpr std::array<u8, 32> expected_hash{
        0xa1,0x62,0x4c,0xc4,0x23,0x8f,0xef,0x04,0x6f,0x30,0xc4,0xd8,0x0c,0xe3,0xf4,0x7b,
        0xe6,0x3f,0xc5,0xf5,0x37,0x3f,0x49,0xe3,0xee,0x9e,0xdb,0x99,0x60,0xf5,0x4c,0x78};
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != std::streampos{9332432}) {
        LOG_WARNING(Render_Vulkan, "FG pacing workaround: DLL size differs; skipped");
        return false;
    }
    std::vector<u8> bytes(9332432);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return false;
    const HMODULE crypto = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!crypto) return false;
    const auto open = reinterpret_cast<decltype(&BCryptOpenAlgorithmProvider)>(GetProcAddress(crypto, "BCryptOpenAlgorithmProvider"));
    const auto hash = reinterpret_cast<decltype(&BCryptHash)>(GetProcAddress(crypto, "BCryptHash"));
    const auto close = reinterpret_cast<decltype(&BCryptCloseAlgorithmProvider)>(GetProcAddress(crypto, "BCryptCloseAlgorithmProvider"));
    BCRYPT_ALG_HANDLE algorithm{};
    std::array<u8, 32> actual{};
    bool matches = false;
    if (open && hash && close && open(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0) {
        matches = hash(algorithm, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()),
                       actual.data(), static_cast<ULONG>(actual.size())) >= 0 && actual == expected_hash;
        close(algorithm, 0);
    }
    FreeLibrary(crypto);
    if (!matches) {
        LOG_WARNING(Render_Vulkan, "FG pacing workaround: DLL hash differs; skipped");
        return false;
    }
    auto* base = reinterpret_cast<u8*>(module);
    constexpr std::array<u8, 9> load{0xf3,0x44,0x0f,0x10,0x0d,0x45,0x34,0x02,0x00};
    constexpr std::array<u8, 12> compare{0xf3,0x41,0x0f,0x59,0xc1,0x0f,0x5a,0xc0,0x66,0x0f,0x2f,0xf0};
    u32 value{};
    std::memcpy(&value, base + 0x4c650, sizeof(value));
    if (std::memcmp(base + 0x29202, load.data(), load.size()) ||
        std::memcmp(base + 0x29387, compare.data(), compare.size()) || value != 0x3dcccccd) {
        LOG_WARNING(Render_Vulkan, "FG pacing workaround: loaded signature differs; skipped");
        return false;
    }
    DWORD old{};
    if (!VirtualProtect(base + 0x4c650, sizeof(value), PAGE_READWRITE, &old)) return false;
    const u32 replacement = 0x3f000000; // 0.5 seconds, still reset after a genuine long gap.
    std::memcpy(base + 0x4c650, &replacement, sizeof(replacement));
    DWORD ignored{};
    if (!VirtualProtect(base + 0x4c650, sizeof(value), old, &ignored)) {
        std::memcpy(base + 0x4c650, &value, sizeof(value));
        VirtualProtect(base + 0x4c650, sizeof(value), old, &ignored);
        LOG_ERROR(Render_Vulkan, "FG pacing workaround: protection restore failed; rolled back");
        return false;
    }
    LOG_INFO(Render_Vulkan, "FG pacing workaround: verified DLL, history timeout 100 -> 500 ms");
    return true;
}

bool Load() {
    if (g_api.tried) {
        return g_api.module != nullptr;
    }
    g_api.tried = true;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir{exe};
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    const std::wstring path = dir + L"\\amd_fidelityfx_vk.dll";
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        LOG_WARNING(Render_Vulkan,
                    "Frame generation: amd_fidelityfx_vk.dll could not be loaded (error {}); off",
                    GetLastError());
        return false;
    }
#define LOAD(field, name)                                                                          \
    g_api.field = reinterpret_cast<decltype(g_api.field)>(                                         \
        reinterpret_cast<void*>(GetProcAddress(module, name)))
    LOAD(create_context, "ffxCreateContext");
    LOAD(destroy_context, "ffxDestroyContext");
    LOAD(configure, "ffxConfigure");
    LOAD(query, "ffxQuery");
    LOAD(dispatch, "ffxDispatch");
#undef LOAD
    if (!g_api.create_context || !g_api.destroy_context || !g_api.configure || !g_api.query ||
        !g_api.dispatch) {
        LOG_WARNING(Render_Vulkan, "Frame generation: amd_fidelityfx_vk.dll is missing functions");
        FreeLibrary(module);
        g_api = Api{.tried = true};
        return false;
    }
    g_api.module = module;
    ApplyLowFpsPacing(module, path);
    return true;
}

void OnMessage(uint32_t type, const wchar_t* message) {
    std::string text;
    for (const wchar_t* c = message; c && *c; ++c) {
        text.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
    }
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        LOG_ERROR(Render_Vulkan, "Frame generation: {}", text);
    } else {
        LOG_WARNING(Render_Vulkan, "Frame generation: {}", text);
    }
}

FfxApiResource Resource(vk::Image image, vk::Format format, vk::Extent2D size, uint32_t usage,
                        uint32_t state) {
    FfxApiResourceDescription description{};
    description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    description.format = ffxApiGetSurfaceFormatVK(static_cast<VkFormat>(format));
    description.width = size.width;
    description.height = size.height;
    description.depth = 1;
    description.mipCount = 1;
    description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    description.usage = usage;
    return ffxApiGetResourceVK(static_cast<VkImage>(image), description, state);
}

// Generation itself runs from FidelityFX's swapchain; this sets how the picture is encoded.
ffxReturnCode_t DispatchGeneration(ffxDispatchDescFrameGeneration* params, void* user) {
    auto* context = static_cast<ffxContext*>(user);
    const bool hdr = params->presentColor.description.format ==
                     FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    params->backbufferTransferFunction =
        hdr ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    params->minMaxLuminance[0] = 0.0001f;
    params->minMaxLuminance[1] = 1000.0f;
    const vk::CommandBuffer cmd{static_cast<VkCommandBuffer>(params->commandList)};
    const int timing = g_timing.Begin(cmd, params->frameID, true);
    const auto result = g_api.dispatch(context, &params->header);
    g_timing.End(cmd, timing);
    return result;
}

} // namespace

std::atomic<u64> g_shown_frames{};
int BeginGpuTiming(vk::CommandBuffer cmd, u64 sequence, u32 stage) {
    return g_timing.BeginExtra(cmd, sequence, stage);
}
void EndGpuTiming(vk::CommandBuffer cmd, int query) {
    g_timing.End(cmd, query);
}
namespace {
std::atomic<s64> g_generated_at{};
struct GenerationWindow {
    std::chrono::steady_clock::time_point start{};
    u64 shown{}, game{};
    u32 confirmations{};
} g_generation_window;
}
void ReportWorking(bool working) {
    const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    g_generated_at.store(working ? stamp : 0, std::memory_order_relaxed);
}
bool Working() {
    const auto stamp = g_generated_at.load(std::memory_order_relaxed);
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return stamp && now >= stamp && now - stamp < 2000000000LL;
}


bool Requested() {
    // NVIDIA DLSS frame generation (ryty_dlssg.h) replaces the AMD one when both are on.
    return EmulatorSettings.IsFrameGenEnabled() && !EmulatorSettings.IsDlssgEnabled();
}

u64 ShownFrames() {
    return g_shown_frames.load(std::memory_order_relaxed);
}

FrameGen::~FrameGen() {
    Destroy();
}

bool FrameGen::CreateSwapchain(const Instance& instance, const vk::SwapchainCreateInfoKHR& info,
                               vk::SwapchainKHR& swapchain) {
    Destroy();
    if (!Requested()) {
        return false;
    }
    const vk::Queue async_queue = instance.GetFrameGenQueue(0);
    const vk::Queue present_queue = instance.GetFrameGenQueue(1);
    const vk::Queue acquire_queue = instance.GetFrameGenQueue(2);
    if (!async_queue || !present_queue || !acquire_queue) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_WARNING(Render_Vulkan, "Frame generation: the GPU's graphics queue family has "
                                       "fewer than 4 queues; off");
        }
        return false;
    }
    if (!Load()) {
        return false;
    }
    device = instance.GetDevice();
    allocator = instance.GetAllocator();
    const u32 family = instance.GetGraphicsQueueFamilyIndex();

    VkSwapchainKHR handle = VK_NULL_HANDLE;
    ffxCreateContextDescFrameGenerationSwapChainVK swapchain_desc{};
    swapchain_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    swapchain_desc.physicalDevice = static_cast<VkPhysicalDevice>(instance.GetPhysicalDevice());
    swapchain_desc.device = static_cast<VkDevice>(device);
    swapchain_desc.swapchain = &handle;
    swapchain_desc.allocator = nullptr;
    swapchain_desc.createInfo = static_cast<VkSwapchainCreateInfoKHR>(info);
    swapchain_desc.gameQueue = {static_cast<VkQueue>(instance.GetGraphicsQueue()), family, nullptr};
    swapchain_desc.asyncComputeQueue = {static_cast<VkQueue>(async_queue), family, nullptr};
    swapchain_desc.presentQueue = {static_cast<VkQueue>(present_queue), family, nullptr};
    swapchain_desc.imageAcquireQueue = {static_cast<VkQueue>(acquire_queue), family, nullptr};
    ffxContext new_swapchain_context{};
    auto result = g_api.create_context(&new_swapchain_context, &swapchain_desc.header, nullptr);
    if (result != FFX_API_RETURN_OK || handle == VK_NULL_HANDLE) {
        LOG_ERROR(Render_Vulkan, "Frame generation: creating its swapchain failed (code {}); off",
                  result);
        return false;
    }
    swapchain_context = new_swapchain_context;

    ffxQueryDescSwapchainReplacementFunctionsVK functions{};
    functions.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    g_api.query(reinterpret_cast<ffxContext*>(&swapchain_context), &functions.header);
    get_images = functions.pOutGetSwapchainImagesKHR;
    acquire = functions.pOutAcquireNextImageKHR;
    present = functions.pOutQueuePresentKHR;
    present_count = reinterpret_cast<void*>(functions.pOutGetLastPresentCountFFXAPI);
    if (!get_images || !acquire || !present) {
        LOG_ERROR(Render_Vulkan, "Frame generation: its swapchain functions are missing; off");
        Destroy();
        return false;
    }

    // Pacing that sleeps between short spins and waits on fences, instead of spinning a core.
    static FfxApiSwapchainFramePacingTuning tuning{0.1f, 0.1f, true, 2, true};
    ffxConfigureDescFrameGenerationSwapChainKeyValueVK pacing{};
    pacing.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_VK;
    pacing.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
    pacing.ptr = &tuning;
    g_api.configure(reinterpret_cast<ffxContext*>(&swapchain_context), &pacing.header);

    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = static_cast<VkDevice>(device);
    backend.vkPhysicalDevice = static_cast<VkPhysicalDevice>(instance.GetPhysicalDevice());
    backend.vkDeviceProcAddr = RytyFsr3::DeviceProcAddrShim();

    ffxCreateContextDescFrameGeneration framegen_desc{};
    framegen_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    framegen_desc.header.pNext = &backend.header;
    framegen_desc.flags = 0;
    framegen_desc.displaySize = {info.imageExtent.width, info.imageExtent.height};
    framegen_desc.maxRenderSize = {info.imageExtent.width, info.imageExtent.height};
    framegen_desc.backBufferFormat = ffxApiGetSurfaceFormatVK(static_cast<VkFormat>(info.imageFormat));
    ffxContext new_framegen_context{};
    result = g_api.create_context(&new_framegen_context, &framegen_desc.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
        LOG_ERROR(Render_Vulkan, "Frame generation: creating it failed (code {}); off", result);
        Destroy();
        return false;
    }
    framegen_context = new_framegen_context;
    g_timing.Create(instance);
    display_size = info.imageExtent;
    format = info.imageFormat;
    frame_id = 0;
    swapchain = vk::SwapchainKHR{handle};

    ffxQueryGetProviderVersion version{};
    version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    g_api.query(reinterpret_cast<ffxContext*>(&framegen_context), &version.header);
    LOG_INFO(Render_Vulkan, "Frame generation: {} on, {}x{} {}",
             version.versionName ? version.versionName : "FSR 3.1", display_size.width,
             display_size.height, vk::to_string(format));
    (void)OnMessage;
    return true;
}

void FrameGen::Destroy() {
    g_shown_frames.store(0, std::memory_order_relaxed);
    ReportWorking(false);
    g_generation_window = {};
    if (framegen_context) {
        ffxContext context = framegen_context;
        g_api.destroy_context(&context, nullptr);
        framegen_context = nullptr;
    }
    if (swapchain_context) {
        // Also destroys the swapchain it made.
        ffxContext context = swapchain_context;
        g_api.destroy_context(&context, nullptr);
        swapchain_context = nullptr;
    }
    get_images = nullptr;
    g_timing.Destroy();
    acquire = nullptr;
    present = nullptr;
}

vk::Result FrameGen::GetImages(vk::Device device_, vk::SwapchainKHR swapchain,
                               std::vector<vk::Image>& images) const {
    u32 count = 0;
    auto result = static_cast<vk::Result>(get_images(static_cast<VkDevice>(device_),
                                                     static_cast<VkSwapchainKHR>(swapchain), &count,
                                                     nullptr));
    if (result != vk::Result::eSuccess) {
        return result;
    }
    std::vector<VkImage> raw(count);
    result = static_cast<vk::Result>(get_images(static_cast<VkDevice>(device_),
                                                static_cast<VkSwapchainKHR>(swapchain), &count,
                                                raw.data()));
    images.assign(raw.begin(), raw.begin() + count);
    return result;
}

vk::Result FrameGen::Acquire(vk::Device device_, vk::SwapchainKHR swapchain,
                             vk::Semaphore semaphore, u32* image_index) const {
    const auto begin = TimingTrace::Now();
    const auto result = static_cast<vk::Result>(acquire(static_cast<VkDevice>(device_),
                                           static_cast<VkSwapchainKHR>(swapchain), UINT64_MAX,
                                           static_cast<VkSemaphore>(semaphore), VK_NULL_HANDLE,
                                           image_index));
    g_timing.Cpu("acquire", begin);
    return result;
}

vk::Result FrameGen::Present(vk::Queue queue, const vk::PresentInfoKHR& info) const {
    const VkPresentInfoKHR raw = info;
    const auto begin = TimingTrace::Now();
    const auto result = static_cast<vk::Result>(present(static_cast<VkQueue>(queue), &raw));
    g_timing.Cpu("present", begin);
    return result;
}

void FrameGen::Prepare(vk::CommandBuffer cmdbuf, vk::SwapchainKHR swapchain, vk::Extent2D size,
                       bool hdr, const SceneInputs* scene, bool fresh_frame) {
    if (!framegen_context) {
        return;
    }
    const bool scene_valid = scene && scene->valid && scene->depth && scene->motion &&
        scene->size.width && scene->size.height && scene->size.width <= display_size.width &&
        scene->size.height <= display_size.height;
    const auto now = std::chrono::steady_clock::now();
    const float delta_ms =
        last_prepare.time_since_epoch().count() == 0
            ? 1000.0f / 30.0f
            : std::chrono::duration<float, std::milli>(now - last_prepare).count();
    last_prepare = now;
    ++frame_id;
    if (present_count) {
        g_shown_frames.store(reinterpret_cast<PFN_getLastPresentCountFFXAPI>(present_count)(
                                 static_cast<VkSwapchainKHR>(swapchain)),
                             std::memory_order_relaxed);
    }
    // Shown frames (game and generated) against game frames, every 10 s.
    if (present_count && now - last_log >= std::chrono::seconds{10}) {
        const u64 presents = reinterpret_cast<PFN_getLastPresentCountFFXAPI>(present_count)(
            static_cast<VkSwapchainKHR>(swapchain));
        if (last_log.time_since_epoch().count() != 0) {
            const double seconds = std::chrono::duration<double>(now - last_log).count();
            LOG_INFO(Render_Vulkan, "Frame generation: {:.1f} frames shown per second, {:.1f} from "
                                    "the game",
                     (presents - logged_presents) / seconds, (frame_id - logged_frames) / seconds);
        }
        last_log = now;
        logged_presents = presents;
        logged_frames = frame_id;
    }

    ffxConfigureDescFrameGeneration config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.swapChain = static_cast<VkSwapchainKHR>(swapchain);
    config.presentCallback = nullptr;
    config.presentCallbackUserContext = nullptr;
    config.frameGenerationCallback = &DispatchGeneration;
    config.frameGenerationCallbackUserContext = &framegen_context;
    // SHADGT_FRAMEGEN_AB=<seconds> (test): generation alternates on and off in one run, with
    // the game's frame rate logged for each.
    static const double ab_period = [] {
        const char* env = std::getenv("SHADGT_FRAMEGEN_AB");
        return env ? std::atof(env) : 0.0;
    }();
    bool enabled = true;
    if (ab_period > 0.0) {
        static const auto ab_start = now;
        static u64 ab_frames[2]{};
        static double ab_seconds[2]{};
        static auto ab_last = now;
        static auto ab_log = now;
        const double t = std::chrono::duration<double>(now - ab_start).count();
        enabled = static_cast<u64>(t / ab_period) % 2 == 0;
        const double dt = std::chrono::duration<double>(now - ab_last).count();
        ab_last = now;
        ++ab_frames[enabled ? 0 : 1];
        ab_seconds[enabled ? 0 : 1] += dt;
        if (now - ab_log >= std::chrono::seconds{20}) {
            ab_log = now;
            LOG_INFO(Render_Vulkan, "Frame generation A/B: game {:.1f} fps with it, {:.1f} without",
                     ab_frames[0] / std::max(0.001, ab_seconds[0]),
                     ab_frames[1] / std::max(0.001, ab_seconds[1]));
            ab_frames[0] = ab_frames[1] = 0;
            ab_seconds[0] = ab_seconds[1] = 0.0;
        }
    }
    // FIX-104: no generated frames while new shaders compile (and for a second after). With
    // generation on during heavy compiles the driver deadlocked: FFX's present thread waited in
    // the driver while pipeline builds and the recorder waited on its locks (October 16, both
    // renderers; the new one compiles its own shaders at first, so it met it at once).
    {
        static std::chrono::steady_clock::time_point last_compile{};
        if (Common::GuestClock::IsPaused() || Vulkan::CompileSlowMotion() ||
            Vulkan::PipelineBuildsRunning() > 0) {
            last_compile = now;
        }
        if (last_compile != std::chrono::steady_clock::time_point{} &&
            now - last_compile < std::chrono::seconds{1}) {
            enabled = false;
        }
    }
    // With temporal FSR, use only frames carrying matching scene inputs. Intro/UI
    // frames have no such inputs and should retain normal presentation pacing.
    // Standalone optical-flow FG keeps its existing fallback when temporal FSR is off.
    const bool scene_ready = !RytyFsr3::TemporalRequested() || scene_valid;
    config.frameGenerationEnabled = enabled && fresh_frame && scene_ready;
    config.allowAsyncWorkloads = false;
    const bool hudless_valid = scene_valid && scene->hudless && scene->hudless_size == size &&
                               scene->hudless_format == format;
    config.HUDLessColor = hudless_valid
        ? Resource(scene->hudless, scene->hudless_format, scene->hudless_size,
                   FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ)
        : FfxApiResource{};
    config.flags = 0;
    config.onlyPresentGenerated = false;
    config.generationRect = {0, 0, static_cast<int32_t>(size.width),
                             static_cast<int32_t>(size.height)};
    config.frameID = frame_id;
    auto result = g_api.configure(reinterpret_cast<ffxContext*>(&framegen_context), &config.header);
    if (result != FFX_API_RETURN_OK) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_ERROR(Render_Vulkan, "Frame generation: configuring failed (code {})", result);
        }
        ReportWorking(false);
        g_generation_window = {};
        return;
    }

    if (!config.frameGenerationEnabled) {
        ReportWorking(false);
        g_generation_window = {};
        static u32 bypass_logged{};
        if (bypass_logged++ < 12)
            LOG_INFO(Render_Vulkan, "Frame generation: bypass prepare (scene ready {}, fresh {}, compile/pause ready {})",
                scene_ready, fresh_frame, enabled);
        return; // no auxiliary allocations or FG compute work for disabled frames
    }
    if (!scene_valid && size != aux_size) CreateAuxImages(cmdbuf, size);

    ffxDispatchDescFrameGenerationPrepareCameraInfo camera{};
    camera.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
    camera.cameraUp[1] = 1.0f;
    camera.cameraRight[0] = 1.0f;
    camera.cameraForward[2] = 1.0f;

    ffxDispatchDescFrameGenerationPrepare prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepare.header.pNext = &camera.header;
    prepare.frameID = frame_id;
    prepare.flags = 0;
    prepare.commandList = static_cast<VkCommandBuffer>(cmdbuf);
    const vk::Extent2D input_size = scene_valid ? scene->size : size;
    prepare.renderSize = {input_size.width, input_size.height};
    prepare.jitterOffset = {scene_valid ? scene->jitter_x : 0.f,
                            scene_valid ? scene->jitter_y : 0.f};
    prepare.motionVectorScale = {scene_valid ? scene->motion_scale_x : 1.f,
                                 scene_valid ? scene->motion_scale_y : 1.f};
    prepare.frameTimeDelta = scene_valid ? scene->frame_time_ms : delta_ms;
    prepare.unused_reset = false;
    prepare.cameraNear = 0.1f;
    prepare.cameraFar = 1000.0f;
    prepare.cameraFovAngleVertical = 1.0f;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = Resource(scene_valid ? scene->depth : vk::Image{depth}, vk::Format::eR32Sfloat, input_size, FFX_API_RESOURCE_USAGE_READ_ONLY,
                             FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    prepare.motionVectors = Resource(scene_valid ? scene->motion : vk::Image{motion_vectors}, vk::Format::eR16G16Sfloat, input_size,
                                     FFX_API_RESOURCE_USAGE_READ_ONLY,
                                     FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    g_timing.Poll(frame_id);
    const int timing = g_timing.Begin(cmdbuf, frame_id, false);
    if (timing >= 0) g_timing.cpu_armed.store(true, std::memory_order_relaxed);
    result = g_api.dispatch(reinterpret_cast<ffxContext*>(&framegen_context), &prepare.header);
    g_timing.End(cmdbuf, timing);
    if (result != FFX_API_RETURN_OK) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_ERROR(Render_Vulkan, "Frame generation: preparing failed (code {})", result);
        }
    }
    if (result != FFX_API_RETURN_OK) {
        ReportWorking(false);
        g_generation_window = {};
    } else if (present_count) {
        const u64 shown = g_shown_frames.load(std::memory_order_relaxed);
        auto& sample = g_generation_window;
        if (sample.start == std::chrono::steady_clock::time_point{} || shown < sample.shown ||
            frame_id < sample.game) {
            sample = {now, shown, frame_id};
        } else if (now - sample.start >= std::chrono::seconds{1}) {
            // More displayed frames than submitted game frames is actual generation.
            // Margin excludes a short presentation-queue drain at the sample boundary.
            const auto game_delta = frame_id - sample.game;
            const auto shown_delta = shown - sample.shown;
            const bool extra = game_delta >= 2 && shown_delta > game_delta + 2;
            const u32 confirmations = extra ? std::min(sample.confirmations + 1, 2u) : 0;
            ReportWorking(confirmations >= 2);
            sample = {now, shown, frame_id, confirmations};
        }
    }
    if (result == FFX_API_RETURN_OK && scene_valid) {
        static u32 scene_logged{};
        if (scene_logged++ < 12)
            LOG_INFO(Render_Vulkan, "Frame generation: real scene {}x{} R32 depth + motion, scene {} FG {}, jitter ({}, {}), enabled {}, HUD-less {}",
                input_size.width, input_size.height, scene->sequence, frame_id,
                scene->jitter_x, scene->jitter_y, config.frameGenerationEnabled, hudless_valid);
    }
    (void)hdr;
}

void FrameGen::CreateAuxImages(vk::CommandBuffer cmdbuf, vk::Extent2D size) {
    // No motion vectors or depth from the game: zero motion and a flat depth image.
    aux_size = size;
    motion_vectors.Destroy();
    depth.Destroy();
    motion_vectors = VideoCore::UniqueImage(device, allocator);
    depth = VideoCore::UniqueImage(device, allocator);
    const auto make = [&](VideoCore::UniqueImage& image, vk::Format image_format) {
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = image_format,
            .extent{.width = size.width, .height = size.height, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
    };
    make(motion_vectors, vk::Format::eR16G16Sfloat);
    make(depth, vk::Format::eR32Sfloat);

    constexpr vk::ImageSubresourceRange range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const std::array<vk::Image, 2> images = {motion_vectors, depth};
    std::array<vk::ImageMemoryBarrier2, 2> to_clear{};
    std::array<vk::ImageMemoryBarrier2, 2> to_read{};
    for (size_t i = 0; i < images.size(); ++i) {
        to_clear[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = images[i],
            .subresourceRange = range,
        };
        to_read[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = images[i],
            .subresourceRange = range,
        };
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(to_clear.size()),
        .pImageMemoryBarriers = to_clear.data(),
    });
    const vk::ClearColorValue zero{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
    for (const vk::Image image : images) {
        cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, zero, range);
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(to_read.size()),
        .pImageMemoryBarriers = to_read.data(),
    });
}

} // namespace Vulkan::RytyFrameGen
