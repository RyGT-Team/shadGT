// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <windows.h>

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>

#include "common/guest_clock.h"
#include "common/logging/log.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "ryty_gpu/ryty_dlssg.h"
#include "ryty_gpu/ryty_framegen.h"
#include "ryty_gpu/ryty_dlssg_options.h"
#include "ryty_gpu/ryty_dlssg_state.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/texture_cache/image.h"

// Streamline's headers (MIT, ryty_gpu/streamline). Its functions are loaded from
// sl.interposer.dll at run time; the header helpers that need the import library are not used.
#include "ryty_gpu/streamline/sl.h"
#include "ryty_gpu/streamline/sl_consts.h"
#include "ryty_gpu/streamline/sl_dlss_g.h"
#include "ryty_gpu/streamline/sl_pcl.h"
#include "ryty_gpu/streamline/sl_reflex.h"

namespace Vulkan::RytyFrameGen {
extern std::atomic<u64> g_shown_frames; // ryty_framegen.cpp, read by the FPS counter
}

namespace Vulkan {
bool RytyGtsInRace();
}

namespace Vulkan::RytyDlssG {

namespace {

using PFunInit = sl::Result (*)(const sl::Preferences&, uint64_t);
using PFunShutdown = sl::Result (*)();
using PFunIsFeatureSupported = sl::Result (*)(sl::Feature, const sl::AdapterInfo&);
using PFunSetTagForFrame = sl::Result (*)(const sl::FrameToken&, const sl::ViewportHandle&,
                                          const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
using PFunSetConstants = sl::Result (*)(const sl::Constants&, const sl::FrameToken&,
                                        const sl::ViewportHandle&);
using PFunGetFeatureFunction = sl::Result (*)(sl::Feature, const char*, void*&);
using PFunGetNewFrameToken = sl::Result (*)(sl::FrameToken*&, const uint32_t*);

struct Streamline {
    HMODULE module{};
    PFunInit init{};
    PFunShutdown shutdown{};
    PFunIsFeatureSupported is_supported{};
    PFunSetTagForFrame set_tag{};
    PFunSetConstants set_constants{};
    PFunGetFeatureFunction get_feature_function{};
    PFunGetNewFrameToken new_frame_token{};
    PFun_slDLSSGSetOptions* dlssg_set_options{};
    PFun_slDLSSGGetState* dlssg_get_state{};
    int last_status = -1;
    PFun_slReflexSetOptions* reflex_set_options{};
    PFun_slPCLSetMarker* pcl_set_marker{};
    PFN_vkGetInstanceProcAddr get_instance_proc_addr{};
    bool tried{};
    bool features_ready{};
    GenerationOptions options{};
    bool fresh_frame{};
    bool reset_pending = true;
    GenerationGate gate{};
    Suspension suspension = Suspension::None;
    GameplayState gameplay{};
    std::mutex telemetry_mutex;
    sl::FrameToken* frame{};
    u32 frame_index{};
    sl::Result last_inputs_error = sl::Result::eOk;
    Clock::time_point reclaim_after{};
    Clock::time_point report_after{};
    u64 state_samples{};
    u64 enabled_samples{};
    u64 presented_frames{};
    u64 multiple_present_samples{};
    std::mutex mutex;
    // Scene resources belong to the presentation Frame and remain alive through present.
    vk::Extent2D aux_size{};
};
Streamline g_sl;

std::wstring DlssDir() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir{exe};
    return dir.substr(0, dir.find_last_of(L"\\/")) + L"\\dlss";
}

std::wstring LogDir() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir{exe};
    return dir.substr(0, dir.find_last_of(L"\\/")) + L"\\user\\log";
}

void SlLog(sl::LogType type, const char* message) {
    if (type == sl::LogType::eError) {
        LOG_ERROR(Render_Vulkan, "Streamline: {}", message ? message : "");
    } else if (type == sl::LogType::eWarn) {
        LOG_WARNING(Render_Vulkan, "Streamline: {}", message ? message : "");
    } else {
        LOG_INFO(Render_Vulkan, "Streamline: {}", message ? message : "");
    }
}

template <typename T>
T* Feature(sl::Feature feature, const char* name) {
    void* function{};
    if (!g_sl.get_feature_function ||
        g_sl.get_feature_function(feature, name, function) != sl::Result::eOk) {
        return nullptr;
    }
    return reinterpret_cast<T*>(function);
}

bool LoadFeatureFunctions() {
    if (g_sl.features_ready) {
        return g_sl.dlssg_set_options != nullptr;
    }
    g_sl.features_ready = true;
    g_sl.dlssg_set_options =
        Feature<PFun_slDLSSGSetOptions>(sl::kFeatureDLSS_G, "slDLSSGSetOptions");
    g_sl.dlssg_get_state = Feature<PFun_slDLSSGGetState>(sl::kFeatureDLSS_G, "slDLSSGGetState");
    g_sl.reflex_set_options =
        Feature<PFun_slReflexSetOptions>(sl::kFeatureReflex, "slReflexSetOptions");
    g_sl.pcl_set_marker = Feature<PFun_slPCLSetMarker>(sl::kFeaturePCL, "slPCLSetMarker");
    if (!g_sl.dlssg_set_options) {
        LOG_ERROR(Render_Vulkan, "ryGT DLSS-G: DLSS frame generation is not available here "
                                 "(see the Streamline lines above: it needs an RTX 40 or newer "
                                 "GPU and Windows' hardware-accelerated GPU scheduling)");
        return false;
    }
    if (g_sl.reflex_set_options) {
        sl::ReflexOptions reflex{};
        reflex.mode = sl::ReflexMode::eLowLatency;
        g_sl.reflex_set_options(reflex);
    }
    return true;
}

sl::float4x4 Identity() {
    sl::float4x4 m;
    m.setRow(0, sl::float4(1, 0, 0, 0));
    m.setRow(1, sl::float4(0, 1, 0, 0));
    m.setRow(2, sl::float4(0, 0, 1, 0));
    m.setRow(3, sl::float4(0, 0, 0, 1));
    return m;
}

void Marker(sl::PCLMarker marker) {
    if (g_sl.pcl_set_marker && g_sl.frame) {
        g_sl.pcl_set_marker(marker, *g_sl.frame);
    }
}

bool SetGenerationMode(bool enabled, VmaAllocator allocator) {
    const auto now = Clock::now();
    const auto update = g_sl.options.Apply(now, enabled, [&](bool target) {
        sl::DLSSGOptions options{};
        options.mode = target ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        options.numFramesToGenerate = 1;
        options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        return g_sl.dlssg_set_options(sl::ViewportHandle{0u}, options);
    });
    if (!update.attempted) {
        return update.applied;
    }
    if (update.result == sl::Result::eWarnOutOfVRAM && now >= g_sl.reclaim_after) {
        g_sl.reclaim_after = now + std::chrono::seconds{30};
        const auto recycled = VideoCore::ReleaseRecycledImages(allocator);
        const auto empty_blocks = Vulkan::TrimEmptyVmaBlocks(allocator);
        LOG_WARNING(Render_Vulkan,
                    "ryGT DLSS-G: memory-budget warning; keeping requested mode, released {} "
                    "recycled bytes and {} empty VMA block bytes; not retrying options",
                    recycled, empty_blocks);
    }
    if (!OptionsAccepted(update.result)) {
        LOG_WARNING(Render_Vulkan,
                    "ryGT DLSS-G: options failed (result {}); clearing frame inputs, "
                    "failed enable retries after 2 s",
                    static_cast<int>(update.result));
        return false;
    }
    LOG_WARNING(Render_Vulkan, "ryGT DLSS-G: frame generation requested {} (options result {})",
                update.enabled ? "on" : "suspended", static_cast<int>(update.result));
    return update.applied;
}

const char* SuspensionName(Suspension reason) {
    switch (reason) {
    case Suspension::Gameplay:
        return "loading, menu, pause or inactive telemetry";
    case Suspension::RepeatedFrame:
        return "repeated frame or presentation gap";
    case Suspension::Compilation:
        return "shader compilation";
    case Suspension::Resize:
        return "output size changed";
    case Suspension::Settling:
        return "waiting for stable gameplay";
    default:
        return "active gameplay";
    }
}

} // namespace

bool Requested() {
    return EmulatorSettings.IsDlssgEnabled();
}

bool Active() {
    return g_sl.get_instance_proc_addr != nullptr;
}

void PublishTelemetry(const u8* packet) {
    std::scoped_lock lock{g_sl.telemetry_mutex};
    g_sl.gameplay.Publish(packet, Clock::now());
}

void SetFreshFrame(bool fresh) {
    if (!Active()) {
        return;
    }
    std::scoped_lock lock{g_sl.mutex};
    g_sl.fresh_frame = fresh;
}

PFN_vkGetInstanceProcAddr LoadInterposer() {
    if (g_sl.tried) {
        return g_sl.get_instance_proc_addr;
    }
    g_sl.tried = true;
    if (!Requested()) {
        return nullptr;
    }
    const std::wstring dir = DlssDir();
    const std::wstring path = dir + L"\\sl.interposer.dll";
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        LOG_ERROR(Render_Vulkan,
                  "ryGT DLSS-G: dlssg_enabled is on but dlss\\sl.interposer.dll could not be "
                  "loaded (error {}); no DLSS frame generation",
                  GetLastError());
        return nullptr;
    }
    const auto get = [&](const char* name) { return GetProcAddress(module, name); };
    g_sl.init = reinterpret_cast<PFunInit>(get("slInit"));
    g_sl.shutdown = reinterpret_cast<PFunShutdown>(get("slShutdown"));
    g_sl.is_supported = reinterpret_cast<PFunIsFeatureSupported>(get("slIsFeatureSupported"));
    g_sl.set_tag = reinterpret_cast<PFunSetTagForFrame>(get("slSetTagForFrame"));
    g_sl.set_constants = reinterpret_cast<PFunSetConstants>(get("slSetConstants"));
    g_sl.get_feature_function =
        reinterpret_cast<PFunGetFeatureFunction>(get("slGetFeatureFunction"));
    g_sl.new_frame_token = reinterpret_cast<PFunGetNewFrameToken>(get("slGetNewFrameToken"));
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(get("vkGetInstanceProcAddr"));
    if (!g_sl.init || !g_sl.set_tag || !g_sl.set_constants || !g_sl.get_feature_function ||
        !g_sl.new_frame_token || !gipa) {
        LOG_ERROR(Render_Vulkan, "ryGT DLSS-G: dlss\\sl.interposer.dll lacks Streamline's API");
        FreeLibrary(module);
        return nullptr;
    }

    static const std::wstring plugin_dir = dir;
    static const std::wstring log_dir = LogDir();
    static const wchar_t* plugin_paths[] = {plugin_dir.c_str()};
    static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex,
                                           sl::kFeaturePCL};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = plugin_paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = log_dir.c_str();
    pref.logMessageCallback = SlLog;
    // Streamline's interposer stands in for vulkan-1.dll (its vkGetInstanceProcAddr gives the
    // driver's functions except for the few it intercepts), and its device proxy adds the
    // extensions, features and queues DLSS-G needs. No over-the-air downloads.
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                 sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = static_cast<u32>(std::size(features));
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "ryGT";
    pref.projectId = "5f1a2b3c-4d5e-4f60-8a7b-9c0d1e2f3a4b";
    pref.renderAPI = sl::RenderAPI::eVulkan;
    const sl::Result result = g_sl.init(pref, sl::kSDKVersion);
    if (result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "ryGT DLSS-G: Streamline did not start (result {}); no DLSS "
                                 "frame generation",
                  static_cast<int>(result));
        FreeLibrary(module);
        return nullptr;
    }
    g_sl.module = module;
    g_sl.get_instance_proc_addr = gipa;
    LOG_WARNING(Render_Vulkan, "ryGT DLSS-G: Streamline started; Vulkan goes through it");
    return gipa;
}

void Prepare(vk::Device device, VmaAllocator allocator, vk::CommandBuffer cmdbuf,
             vk::Extent2D size, const RytyFrameGen::SceneInputs* scene) {
    if (!Active()) {
        return;
    }
    std::scoped_lock lock{g_sl.mutex};
    if (!LoadFeatureFunctions()) {
        return;
    }
    const auto now = Clock::now();
    bool gameplay_ready;
    {
        std::scoped_lock telemetry_lock{g_sl.telemetry_mutex};
        gameplay_ready = g_sl.gameplay.Ready(now);
    }
    const bool scene_valid = scene && scene->valid && scene->size.width && scene->size.height &&
        scene->depth && scene->motion && scene->depth_view && scene->motion_view &&
        scene->hudless && scene->hudless_view && scene->hudless_size == size;
    gameplay_ready = gameplay_ready && scene_valid && RytyGtsInRace() &&
                     !Common::GuestClock::IsPaused() && !DebugState.IsGuestThreadsPaused();
    const bool resized = size != g_sl.aux_size;
    const auto reason =
        g_sl.gate.Update(now, gameplay_ready, std::exchange(g_sl.fresh_frame, false),
                         CompilationInProgress() || CompileSlowMotion(), resized);
    const bool enabled = reason == Suspension::None;
    if (reason != g_sl.suspension) {
        g_sl.suspension = reason;
        LOG_WARNING(Render_Vulkan, "ryGT DLSS-G: {}", SuspensionName(reason));
    }
    if (!enabled) {
        g_sl.reset_pending = true;
    }
    if (resized) {
        g_sl.aux_size = size;
        g_sl.reset_pending = true;
    }
    ++g_sl.frame_index;
    if (g_sl.new_frame_token(g_sl.frame, &g_sl.frame_index) != sl::Result::eOk || !g_sl.frame) {
        g_sl.frame = nullptr;
        g_sl.reset_pending = true;
        SetGenerationMode(false, allocator);
        return;
    }
    Marker(sl::PCLMarker::eSimulationStart);
    Marker(sl::PCLMarker::eSimulationEnd);
    Marker(sl::PCLMarker::eRenderSubmitStart);

    const sl::ViewportHandle viewport{0u};
    sl::Constants constants{};
    constants.cameraViewToClip = Identity();
    constants.clipToCameraView = Identity();
    constants.clipToLensClip = Identity();
    constants.clipToPrevClip = Identity();
    constants.prevClipToClip = Identity();
    constants.jitterOffset = scene_valid ? sl::float2(scene->jitter_x, scene->jitter_y)
                                       : sl::float2(0.0f, 0.0f);
    constants.mvecScale = scene_valid
        ? sl::float2(scene->motion_scale_x / scene->size.width,
                     scene->motion_scale_y / scene->size.height) : sl::float2(1.0f, 1.0f);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(0.0f, 0.0f, 0.0f);
    constants.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
    constants.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
    constants.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
    constants.cameraNear = 0.1f;
    constants.cameraFar = 10000.0f;
    constants.cameraFOV = 1.0f;
    constants.cameraAspectRatio = float(size.width) / float(std::max(size.height, 1u));
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = g_sl.reset_pending ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    const auto constants_result = g_sl.set_constants(constants, *g_sl.frame, viewport);
    const bool valid_inputs = enabled && constants_result == sl::Result::eOk;

    const sl::Extent extent{0, 0, size.width, size.height};
    sl::Resource mvec{sl::ResourceType::eTex2d,
                      static_cast<VkImage>(scene_valid ? scene->motion : vk::Image{}), nullptr,
                      static_cast<VkImageView>(scene_valid ? scene->motion_view : vk::ImageView{}),
                      static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
    const auto render_size = scene_valid ? scene->size : size;
    const sl::Extent render_extent{0, 0, render_size.width, render_size.height};
    mvec.width = render_size.width;
    mvec.height = render_size.height;
    mvec.nativeFormat = static_cast<uint32_t>(VK_FORMAT_R16G16_SFLOAT);
    mvec.mipLevels = 1;
    mvec.arrayLayers = 1;
    mvec.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sl::Resource dep{sl::ResourceType::eTex2d,
                     static_cast<VkImage>(scene_valid ? scene->depth : vk::Image{}), nullptr,
                     static_cast<VkImageView>(scene_valid ? scene->depth_view : vk::ImageView{}),
                     static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
    dep.width = render_size.width;
    dep.height = render_size.height;
    dep.nativeFormat = static_cast<uint32_t>(VK_FORMAT_R32_SFLOAT);
    dep.mipLevels = 1;
    dep.arrayLayers = 1;
    dep.usage = mvec.usage;
    // The backbuffer tag only carries the extent (Streamline knows the swapchain image).
    sl::Resource backbuffer{sl::ResourceType::eTex2d, nullptr, nullptr, nullptr};
    sl::Resource hudless{sl::ResourceType::eTex2d,
        static_cast<VkImage>(scene_valid ? scene->hudless : vk::Image{}), nullptr,
        static_cast<VkImageView>(scene_valid ? scene->hudless_view : vk::ImageView{}),
        static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
    hudless.width = size.width; hudless.height = size.height;
    hudless.nativeFormat = scene_valid ? static_cast<uint32_t>(scene->hudless_format) : 0;
    hudless.mipLevels = 1; hudless.arrayLayers = 1;
    hudless.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const sl::ResourceTag tags[] = {
        sl::ResourceTag{valid_inputs ? &backbuffer : nullptr, sl::kBufferTypeBackbuffer,
                        sl::ResourceLifecycle::eValidUntilPresent, &extent},
        sl::ResourceTag{valid_inputs ? &mvec : nullptr, sl::kBufferTypeMotionVectors,
                        sl::ResourceLifecycle::eValidUntilPresent, &render_extent},
        sl::ResourceTag{valid_inputs ? &dep : nullptr, sl::kBufferTypeDepth,
                        sl::ResourceLifecycle::eValidUntilPresent, &render_extent},
        sl::ResourceTag{valid_inputs ? &hudless : nullptr, sl::kBufferTypeHUDLessColor,
                        sl::ResourceLifecycle::eValidUntilPresent, &extent},
    };
    auto* command_buffer =
        reinterpret_cast<sl::CommandBuffer*>(static_cast<VkCommandBuffer>(cmdbuf));
    const auto tags_result = g_sl.set_tag(*g_sl.frame, viewport, tags,
                                        static_cast<u32>(std::size(tags)), command_buffer);
    const auto inputs_error =
        constants_result != sl::Result::eOk ? constants_result : tags_result;
    if (inputs_error != sl::Result::eOk && inputs_error != g_sl.last_inputs_error) {
        LOG_ERROR(Render_Vulkan, "ryGT DLSS-G: frame inputs rejected (result {}); generation off",
                  static_cast<int>(inputs_error));
    }
    g_sl.last_inputs_error = inputs_error;
    const bool ready = valid_inputs && tags_result == sl::Result::eOk;
    const bool mode_set = SetGenerationMode(ready, allocator);
    if (valid_inputs && !(ready && mode_set)) {
        auto cleared_tags = std::to_array(tags);
        for (auto& tag : cleared_tags) {
            tag.resource = nullptr;
        }
        g_sl.set_tag(*g_sl.frame, viewport, cleared_tags.data(),
                     static_cast<u32>(cleared_tags.size()), command_buffer);
    }
    g_sl.reset_pending = !(ready && mode_set);
    if (!(ready && mode_set)) RytyFrameGen::ReportWorking(false);
    Marker(sl::PCLMarker::eRenderSubmitEnd);

    // Frames shown (game and generated) for the FPS counter, and DLSS-G's status when it changes.
    if (g_sl.dlssg_get_state) {
        sl::DLSSGState state{};
        if (g_sl.dlssg_get_state(viewport, state, nullptr) == sl::Result::eOk) {
            if (ready && mode_set && state.numFramesActuallyPresented > 1)
                RytyFrameGen::ReportWorking(true);
            RytyFrameGen::g_shown_frames.fetch_add(state.numFramesActuallyPresented,
                                                   std::memory_order_relaxed);
            if (static_cast<int>(state.status) != g_sl.last_status) {
                g_sl.last_status = static_cast<int>(state.status);
                LOG_WARNING(Render_Vulkan, "ryGT DLSS-G: status {:#x} (0 = no reported error)",
                            g_sl.last_status);
            }
            ++g_sl.state_samples;
            g_sl.enabled_samples += ready && mode_set;
            g_sl.presented_frames += state.numFramesActuallyPresented;
            g_sl.multiple_present_samples += state.numFramesActuallyPresented > 1;
            if (now >= g_sl.report_after) {
                LOG_INFO(Render_Vulkan,
                         "ryGT DLSS-G: presentation sample: {} queries, {} enabled queries, "
                         "{} reported presented frames, {} queries with multiple presents",
                         g_sl.state_samples, g_sl.enabled_samples, g_sl.presented_frames,
                         g_sl.multiple_present_samples);
                g_sl.report_after = now + std::chrono::seconds{5};
                g_sl.state_samples = 0;
                g_sl.enabled_samples = 0;
                g_sl.presented_frames = 0;
                g_sl.multiple_present_samples = 0;
            }
        }
    }
}

void PresentStart() {
    if (Active()) {
        std::scoped_lock lock{g_sl.mutex};
        Marker(sl::PCLMarker::ePresentStart);
    }
}

void PresentEnd() {
    if (Active()) {
        std::scoped_lock lock{g_sl.mutex};
        Marker(sl::PCLMarker::ePresentEnd);
    }
}

void Shutdown() {
    std::scoped_lock lock{g_sl.mutex};
    RytyFrameGen::ReportWorking(false);
    g_sl.aux_size = {};
    g_sl.options = {};
    g_sl.fresh_frame = false;
    g_sl.reset_pending = true;
    g_sl.gate = {};
    g_sl.reclaim_after = {};
    g_sl.report_after = {};
    g_sl.state_samples = 0;
    g_sl.enabled_samples = 0;
    g_sl.presented_frames = 0;
    g_sl.multiple_present_samples = 0;
}

} // namespace Vulkan::RytyDlssG
