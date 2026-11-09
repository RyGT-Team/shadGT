// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#include "common/logging/log.h"
#include "common/ryty_config.h"
#include "core/emulator_settings.h"
#include "ryty_gpu/ryty_xess.h"

#include "ryty_gpu/xess_api/xess_vk.h"

namespace Vulkan::RytyXess {

namespace {

struct Api {
    HMODULE module{};
    decltype(&xessGetVersion) get_version{};
    decltype(&xessVKGetRequiredInstanceExtensions) instance_extensions{};
    decltype(&xessVKGetRequiredDeviceExtensions) device_extensions{};
    decltype(&xessVKCreateContext) create_context{};
    decltype(&xessVKInit) init{};
    decltype(&xessVKExecute) execute{};
    decltype(&xessGetOptimalInputResolution) optimal_input{};
    decltype(&xessSetVelocityScale) velocity_scale{};
    decltype(&xessSetLoggingCallback) logging{};
    decltype(&xessDestroyContext) destroy_context{};
    bool tried{};
};
Api g_api;
std::mutex g_load_mutex;

std::filesystem::path ExeDir() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path{exe}.parent_path();
}

void OnMessage(const char* message, xess_logging_level_t level) {
    if (level >= XESS_LOGGING_LEVEL_ERROR) {
        LOG_ERROR(Render_Vulkan, "XeSS: {}", message ? message : "");
    } else {
        LOG_WARNING(Render_Vulkan, "XeSS: {}", message ? message : "");
    }
}

xess_vk_image_view_info View(vk::Image image, vk::ImageView view, vk::Format format,
                             vk::Extent2D size,
                             vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor) {
    xess_vk_image_view_info info{};
    info.image = static_cast<VkImage>(image);
    info.imageView = static_cast<VkImageView>(view);
    info.format = static_cast<VkFormat>(format);
    info.width = size.width;
    info.height = size.height;
    info.subresourceRange = {static_cast<VkImageAspectFlags>(aspect), 0, 1, 0, 1};
    return info;
}

} // namespace

bool Requested() {
    static const bool requested = [] {
        std::string value;
        if (const char* env = std::getenv("SHADGT_FSR_UPSCALER")) {
            value = env;
        } else if (const auto config = Common::RytyGameConfigValue("fsr_upscaler")) {
            value = *config;
        }
        return value.find("xess") != std::string::npos && RytyFsr3::TemporalRequested();
    }();
    return requested;
}

bool Load() {
    std::scoped_lock lock{g_load_mutex};
    if (g_api.tried) {
        return g_api.module != nullptr;
    }
    g_api.tried = true;
    if (!Requested()) {
        return false;
    }
    const auto path = ExeDir() / "libxess.dll";
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        LOG_WARNING(Render_Vulkan, "XESS-200: XeSS is selected but libxess.dll could not be loaded "
                    "from next to GTSport.exe (error {}); using FSR",
                    GetLastError());
        return false;
    }
#define LOAD(field, name)                                                                          \
    g_api.field = reinterpret_cast<decltype(g_api.field)>(                                         \
        reinterpret_cast<void*>(GetProcAddress(module, name)))
    LOAD(get_version, "xessGetVersion");
    LOAD(instance_extensions, "xessVKGetRequiredInstanceExtensions");
    LOAD(device_extensions, "xessVKGetRequiredDeviceExtensions");
    LOAD(create_context, "xessVKCreateContext");
    LOAD(init, "xessVKInit");
    LOAD(execute, "xessVKExecute");
    LOAD(optimal_input, "xessGetOptimalInputResolution");
    LOAD(velocity_scale, "xessSetVelocityScale");
    LOAD(logging, "xessSetLoggingCallback");
    LOAD(destroy_context, "xessDestroyContext");
#undef LOAD
    if (!g_api.get_version || !g_api.instance_extensions || !g_api.device_extensions ||
        !g_api.create_context || !g_api.init || !g_api.execute || !g_api.optimal_input ||
        !g_api.velocity_scale || !g_api.destroy_context) {
        LOG_WARNING(Render_Vulkan, "XESS-200: libxess.dll is missing functions; using FSR");
        FreeLibrary(module);
        g_api = Api{.tried = true};
        return false;
    }
    g_api.module = module;
    LOG_INFO(Render_Vulkan, "XESS-200: {} loaded", VersionName());
    return true;
}

std::string VersionName() {
    xess_version_t version{};
    if (!g_api.get_version || g_api.get_version(&version) != XESS_RESULT_SUCCESS) {
        return "XeSS";
    }
    return fmt::format("XeSS {}.{}.{}", version.major, version.minor, version.patch);
}

void AddInstanceExtensions(std::vector<const char*>& extensions,
                           const std::vector<vk::ExtensionProperties>& available) {
    if (!Load()) {
        return;
    }
    uint32_t count = 0;
    const char* const* names = nullptr;
    uint32_t api_version = 0;
    if (g_api.instance_extensions(&count, &names, &api_version) != XESS_RESULT_SUCCESS) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const bool offered = std::ranges::any_of(available, [&](const vk::ExtensionProperties& p) {
            return std::strcmp(p.extensionName, names[i]) == 0;
        });
        const bool present = std::ranges::any_of(
            extensions, [&](const char* e) { return std::strcmp(e, names[i]) == 0; });
        if (offered && !present) {
            extensions.push_back(names[i]);
        }
        LOG_INFO(Render_Vulkan, "XESS-200: instance extension {}{}", names[i],
                 offered ? "" : " (not offered)");
    }
}

void AddDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical_device,
                         const std::function<bool(std::string_view)>& add_extension) {
    if (!Load()) {
        return;
    }
    uint32_t count = 0;
    const char* const* names = nullptr;
    if (g_api.device_extensions(static_cast<VkInstance>(instance),
                                static_cast<VkPhysicalDevice>(physical_device), &count,
                                &names) != XESS_RESULT_SUCCESS) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const bool added = add_extension(names[i]);
        LOG_INFO(Render_Vulkan, "XESS-200: device extension {}{}", names[i],
                 added ? "" : " (not offered)");
    }
}

Temporal::~Temporal() {
    if (context && g_api.destroy_context) {
        g_api.destroy_context(static_cast<xess_context_handle_t>(context));
    }
}

bool Temporal::Evaluate(vk::Instance instance, vk::PhysicalDevice physical_device,
                        vk::Device device, vk::CommandBuffer cmdbuf,
                        const RytyFsr3::TemporalFrame& frame) {
    if (!Load()) {
        return false;
    }
    if (!context || frame.render_size != context_input || frame.output_size != context_output) {
        if (context) {
            g_api.destroy_context(static_cast<xess_context_handle_t>(context));
            context = nullptr;
        }
        xess_context_handle_t handle{};
        if (const auto result = g_api.create_context(static_cast<VkInstance>(instance),
                                                     static_cast<VkPhysicalDevice>(physical_device),
                                                     static_cast<VkDevice>(device), &handle);
            result != XESS_RESULT_SUCCESS) {
            LOG_ERROR(Render_Vulkan, "XESS-200: creating the context failed ({})", int(result));
            return false;
        }
        if (g_api.logging) {
            g_api.logging(handle, XESS_LOGGING_LEVEL_WARNING, OnMessage);
        }
        // The quality mode for this output (the scene is always GT Sport's 1920x1080): the one
        // whose nominal scale is nearest the actual one (FSR-209: the launcher's preset), among
        // those whose input range holds the scene's size.
        const xess_2d_t output{frame.output_size.width, frame.output_size.height};
        std::array<std::pair<xess_quality_settings_t, float>, 7> qualities{{
            {XESS_QUALITY_SETTING_AA, 1.0f},
            {XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS, 1.3f},
            {XESS_QUALITY_SETTING_ULTRA_QUALITY, 1.5f},
            {XESS_QUALITY_SETTING_QUALITY, 1.7f},
            {XESS_QUALITY_SETTING_BALANCED, 2.0f},
            {XESS_QUALITY_SETTING_PERFORMANCE, 2.3f},
            {XESS_QUALITY_SETTING_ULTRA_PERFORMANCE, 3.0f},
        }};
        const float scale = float(output.x) / float(frame.render_size.width);
        std::ranges::stable_sort(qualities, {}, [scale](const auto& entry) {
            return std::abs(entry.second - scale);
        });
        xess_quality_settings_t quality = XESS_QUALITY_SETTING_AA;
        bool fits = false;
        for (const auto [candidate, nominal] : qualities) {
            xess_2d_t optimal{}, minimum{}, maximum{};
            if (g_api.optimal_input(handle, &output, candidate, &optimal, &minimum, &maximum) ==
                    XESS_RESULT_SUCCESS &&
                frame.render_size.width >= minimum.x && frame.render_size.height >= minimum.y &&
                frame.render_size.width <= maximum.x && frame.render_size.height <= maximum.y) {
                quality = candidate;
                fits = true;
                break;
            }
        }
        if (!fits) {
            LOG_WARNING(Render_Vulkan, "XESS-200: no quality mode takes {}x{} to {}x{}; keeping the "
                        "game's TAA", frame.render_size.width, frame.render_size.height,
                        output.x, output.y);
            g_api.destroy_context(handle);
            return false;
        }
        xess_vk_init_params_t init{};
        init.outputResolution = output;
        init.qualitySetting = quality;
        // Like the DLSS pass: display-encoded color, depth not inverted, motion vectors at the
        // render size without jitter.
        init.initFlags = XESS_INIT_FLAG_LDR_INPUT_COLOR;
        if (const auto result = g_api.init(handle, &init); result != XESS_RESULT_SUCCESS) {
            LOG_ERROR(Render_Vulkan, "XESS-200: initializing failed ({})", int(result));
            g_api.destroy_context(handle);
            return false;
        }
        context = handle;
        context_input = frame.render_size;
        context_output = frame.output_size;
        LOG_INFO(Render_Vulkan, "XESS-200: {} temporal for the scene, {}x{} to {}x{} (quality {})",
                 VersionName(), frame.render_size.width, frame.render_size.height, output.x,
                 output.y, int(quality));
    }
    auto* handle = static_cast<xess_context_handle_t>(context);
    g_api.velocity_scale(handle, frame.motion_scale_x, frame.motion_scale_y);

    const vk::MemoryBarrier2 inputs_ready{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &inputs_ready,
    });
    xess_vk_execute_params_t params{};
    params.colorTexture = View(frame.color, frame.color_view, frame.color_format, frame.render_size);
    params.velocityTexture =
        View(frame.motion, frame.motion_view, frame.motion_format, frame.render_size);
    params.depthTexture = View(frame.depth, frame.depth_view, frame.depth_format, frame.render_size);
    params.outputTexture =
        View(frame.output, frame.output_view, frame.output_format, frame.output_size);
    params.jitterOffsetX = std::clamp(frame.jitter_x, -0.5f, 0.5f);
    params.jitterOffsetY = std::clamp(frame.jitter_y, -0.5f, 0.5f);
    params.exposureScale = 1.0f;
    params.resetHistory = frame.reset ? 1u : 0u;
    params.inputWidth = frame.render_size.width;
    params.inputHeight = frame.render_size.height;
    if (const auto result =
            g_api.execute(handle, static_cast<VkCommandBuffer>(cmdbuf), &params);
        result != XESS_RESULT_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "XESS-200: upscaling failed ({})", int(result));
        return false;
    }
    return true;
}

} // namespace Vulkan::RytyXess
