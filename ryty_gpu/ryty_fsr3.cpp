// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common/logging/log.h"
#include "common/ryty_config.h"
#include "core/emulator_settings.h"
#include "ryty_gpu/ryty_dlss.h"
#include "ryty_gpu/ryty_fsr3.h"
#include "video_core/renderer_vulkan/vk_platform.h"

#include "ryty_gpu/ffx_api/ffx_api.h"
#include "ryty_gpu/ffx_api/ffx_upscale.h"
#include "ryty_gpu/ffx_api/vk/ffx_api_vk.h"

namespace Vulkan::RytyFsr3 {

namespace {

struct Api {
    HMODULE module{};
    PfnFfxCreateContext create_context{};
    PfnFfxDestroyContext destroy_context{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};
    bool tried{};
};
Api g_api;

// FSR-204: the provider version of the upscaler created last.
std::mutex g_version_mutex;
std::string g_version_name{"FSR"};

void NoteVersion(const char* name) {
    std::scoped_lock lock{g_version_mutex};
    const std::string_view view{name ? name : ""};
    g_version_name = view.empty() ? "FSR" : view.starts_with("FSR") ? std::string{view} : "FSR " + std::string{view};
}

// The context is created for windows up to this size, so resizing rarely recreates it.
constexpr vk::Extent2D MinUpscaleSize{3840, 2160};

void OnMessage(uint32_t type, const wchar_t* message) {
    std::string text;
    for (const wchar_t* c = message; c && *c; ++c) {
        text.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
    }
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        LOG_ERROR(Render_Vulkan, "FSR 3.1: {}", text);
    } else {
        LOG_WARNING(Render_Vulkan, "FSR 3.1: {}", text);
    }
}

// AMD's backend loads some functions by their extension names (vkGetBufferMemoryRequirements2KHR)
// or from VK_EXT_debug_utils, which the renderer's device may not have enabled, and then calls them
// without checking. Extension names fall back to the core Vulkan function; debug labels do nothing.
void VKAPI_CALL NoBeginLabel(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
void VKAPI_CALL NoEndLabel(VkCommandBuffer) {}
VkResult VKAPI_CALL NoObjectName(VkDevice, const VkDebugUtilsObjectNameInfoEXT*) {
    return VK_SUCCESS;
}

PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
    const auto real = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    if (const auto function = real(device, name)) {
        return function;
    }
    const std::string_view view{name};
    if (view.ends_with("KHR")) {
        const std::string core{view.substr(0, view.size() - 3)};
        if (const auto function = real(device, core.c_str())) {
            return function;
        }
    }
    if (view == "vkCmdBeginDebugUtilsLabelEXT") {
        return reinterpret_cast<PFN_vkVoidFunction>(&NoBeginLabel);
    }
    if (view == "vkCmdEndDebugUtilsLabelEXT") {
        return reinterpret_cast<PFN_vkVoidFunction>(&NoEndLabel);
    }
    if (view == "vkSetDebugUtilsObjectNameEXT") {
        return reinterpret_cast<PFN_vkVoidFunction>(&NoObjectName);
    }
    LOG_INFO(Render_Vulkan, "FSR 3.1: {} is not available", view);
    return nullptr;
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

bool g_sdk2 = false;
constexpr uint64_t SDK2_BACKEND_VK = 0x01000003u;

bool LoadApi() {
    if (g_api.tried) {
        return g_api.module != nullptr;
    }
    g_api.tried = true;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir{exe};
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    const std::wstring path = dir + L"\\amd_fidelityfx_vk.dll";
    // FSR-210: SDK 2.x splits the effects; its loader finds amd_fidelityfx_upscaler_vk.dll beside
    // it. Older builds keep the single amd_fidelityfx_vk.dll.
    const std::wstring loader = dir + L"\\amd_fidelityfx_loader_vk.dll";
    HMODULE module = LoadLibraryExW(loader.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    g_sdk2 = module != nullptr;
    if (!module) {
        module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (!module) {
        LOG_WARNING(Render_Vulkan,
                    "FSR 3.1: fsr3_enabled is on but amd_fidelityfx_vk.dll could not be loaded "
                    "(error {}); using FSR 1",
                    GetLastError());
        return false;
    }
#define LOAD(field, name)                                                                          \
    g_api.field = reinterpret_cast<decltype(g_api.field)>(                                         \
        reinterpret_cast<void*>(GetProcAddress(module, name)))
    LOAD(create_context, "ffxCreateContext");
    LOAD(destroy_context, "ffxDestroyContext");
    LOAD(query, "ffxQuery");
    LOAD(dispatch, "ffxDispatch");
#undef LOAD
    if (!g_api.create_context || !g_api.destroy_context || !g_api.query || !g_api.dispatch) {
        LOG_WARNING(Render_Vulkan, "FSR 3.1: amd_fidelityfx_vk.dll is missing functions; using FSR 1");
        FreeLibrary(module);
        g_api = Api{.tried = true};
        return false;
    }
    g_api.module = module;
    return true;
}

// FSR-210: SDK 2.x renumbered the Vulkan backend descriptor; its fields stayed the same.
uint64_t BackendDescType() {
    return g_sdk2 ? SDK2_BACKEND_VK : FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
}

// FSR-201: the upscaler major version GPU.fsr_upscaler asks for (3 or 4).
u32 RequestedMajor() {
    static const u32 major = [] {
        std::string value;
        if (const char* env = std::getenv("SHADGT_FSR_UPSCALER")) {
            value = env;
        } else if (const auto config = Common::RytyGameConfigValue("fsr_upscaler")) {
            value = *config;
        }
        return value.find('4') != std::string::npos ? 4u : 3u;
    }();
    return major;
}

// FSR-201: the version id to create upscalers with: the newest one of the requested major version
// in the DLL, else the newest 3.x. Zero leaves the DLL's default.
uint64_t SelectedVersion(const ffxCreateBackendVKDesc& backend) {
    static const uint64_t selected = [&] {
        uint64_t count = 0;
        ffxQueryDescGetVersions query{};
        query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        // FSR-210: SDK 2.x lists driver-provided versions (FSR 4) only for a given device. The
        // three handles are laid out as its VkDeviceContext.
        // FSR-211: only when FSR 4 is asked for, on a card AMD's driver runs it on (RX 9000,
        // and RX 7000 and RX 6000 with the INT8 FSR 4.1 model), and with a
        // FidelityFX DLL from SDK 2.x or newer (Vulkan FSR 4 came with SDK 2.4). With a device
        // the DLL asks the AMD driver (amdxc64.dll) for its FSR 4 provider; through the 1.0.x
        // DLL this build shipped, that call jumped to an invalid address on an RX 7600 and GT
        // Sport crashed the first time FSR ran. Without one the DLL lists its own versions.
        const auto properties = vk::PhysicalDevice{backend.vkPhysicalDevice}.getProperties();
        const std::string_view gpu_name{properties.deviceName.data()};
        const bool fsr4_gpu = properties.vendorID == 0x1002 &&
                              (gpu_name.find("RX 9") != std::string_view::npos ||
                               gpu_name.find("RX 7") != std::string_view::npos ||
                               gpu_name.find("RX 6") != std::string_view::npos);
        if (RequestedMajor() == 4 && fsr4_gpu && g_sdk2) {
            query.device = const_cast<VkDevice*>(&backend.vkDevice);
        } else if (RequestedMajor() == 4) {
            LOG_WARNING(Render_Vulkan, "FSR-211: FSR 4 needs a Radeon RX 6000, RX 7000 or RX 9000 card "
                                       "(this is {}) and FidelityFX SDK 2.x DLLs "
                                       "(amd_fidelityfx_loader_vk.dll; this build has {}); FSR "
                                       "3.1 is used",
                        gpu_name, g_sdk2 ? "them" : "only the 1.x amd_fidelityfx_vk.dll");
        }
        query.outputCount = &count;
        if (g_api.query(nullptr, &query.header) != FFX_API_RETURN_OK || count == 0) {
            return uint64_t{0};
        }
        std::vector<uint64_t> ids(count);
        std::vector<const char*> names(count);
        query.versionIds = ids.data();
        query.versionNames = names.data();
        if (g_api.query(nullptr, &query.header) != FFX_API_RETURN_OK) {
            return uint64_t{0};
        }
        const auto major_of = [](const char* name) {
            return name ? static_cast<u32>(std::strtoul(name, nullptr, 10)) : 0u;
        };
        // The DLL lists its versions newest first.
        const auto newest = [&](u32 major) {
            for (u64 i = 0; i < count; ++i) {
                if (major_of(names[i]) == major) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        std::string listed;
        for (u64 i = 0; i < count; ++i) {
            listed += fmt::format("{}{}", i ? ", " : "", names[i] ? names[i] : "?");
        }
        int pick = newest(RequestedMajor());
        if (pick < 0 && RequestedMajor() == 4) {
            LOG_WARNING(Render_Vulkan,
                        "FSR-201: FSR 4 is not in this amd_fidelityfx_vk.dll (it has {}); AMD has "
                        "not released it for Vulkan yet, so FSR 3.1 is used",
                        listed);
        }
        if (pick < 0) {
            pick = newest(3);
        }
        if (pick < 0) {
            return uint64_t{0};
        }
        LOG_INFO(Render_Vulkan, "FSR-201: upscaler versions in the DLL: {}; using {}", listed,
                 names[pick] ? names[pick] : "?");
        return ids[pick];
    }();
    return selected;
}

} // namespace

std::string ActiveVersionName() {
    std::scoped_lock lock{g_version_mutex};
    return g_version_name;
}

PFN_vkGetDeviceProcAddr DeviceProcAddrShim() {
    return &GetDeviceProcAddr;
}

Pass::~Pass() {
    DestroyContext();
}

bool Pass::Load() {
    return LoadApi();
}

void Pass::Create(vk::Instance, vk::PhysicalDevice physical_device_, vk::Device device_,
                  VmaAllocator allocator_, u32 num_images) {
    physical_device = physical_device_;
    device = device_;
    allocator = allocator_;
    available_imgs.resize(num_images);
    for (u32 i = 0; i < num_images; ++i) {
        available_imgs[i].id = static_cast<u8>(i);
        available_imgs[i].output_image = VideoCore::UniqueImage(device, allocator);
    }
    motion_vectors = VideoCore::UniqueImage(device, allocator);
    depth = VideoCore::UniqueImage(device, allocator);
}

bool Pass::CreateContext(vk::Extent2D input_size, vk::Extent2D output_size) {
    DestroyContext();
    const vk::Extent2D max_output{std::max(output_size.width, MinUpscaleSize.width),
                                  std::max(output_size.height, MinUpscaleSize.height)};
    ffxCreateBackendVKDesc backend{};
    backend.header.type = BackendDescType();
    backend.vkDevice = static_cast<VkDevice>(device);
    backend.vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical_device);
    backend.vkDeviceProcAddr = &GetDeviceProcAddr;

    ffxOverrideVersion version_override{};
    version_override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    version_override.header.pNext = &backend.header;
    version_override.versionId = SelectedVersion(backend);

    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = version_override.versionId ? &version_override.header : &backend.header;
    desc.flags = 0; // the game picture is display-ready (LDR) color
    desc.maxRenderSize = {input_size.width, input_size.height};
    desc.maxUpscaleSize = {max_output.width, max_output.height};
    desc.fpMessage = OnMessage;

    ffxContext new_context{};
    const auto result = g_api.create_context(&new_context, &desc.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
        LOG_ERROR(Render_Vulkan, "FSR 3.1: creating the upscaler failed (code {}); using FSR 1",
                  result);
        return false;
    }
    context = new_context;
    context_input = input_size;
    context_output = max_output;

    ffxQueryGetProviderVersion version{};
    version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    FfxApiEffectMemoryUsage memory{};
    ffxQueryDescUpscaleGetGPUMemoryUsage memory_query{};
    memory_query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
    memory_query.gpuMemoryUsageUpscaler = &memory;
    g_api.query(&context, &version.header);
    g_api.query(&context, &memory_query.header);
    NoteVersion(version.versionName);
    LOG_INFO(Render_Vulkan, "FSR 3.1: upscaler {} ready for {}x{} to {}x{} ({} MB of GPU memory)",
             version.versionName ? version.versionName : "(unknown version)", input_size.width,
             input_size.height, max_output.width, max_output.height,
             memory.totalUsageInBytes >> 20);
    return true;
}

void Pass::DestroyContext() {
    if (context && g_api.destroy_context) {
        g_api.destroy_context(&context, nullptr);
    }
    context = nullptr;
}

vk::ImageView Pass::Render(vk::CommandBuffer cmdbuf, vk::Image input_image, vk::ImageView input,
                           vk::Format input_format, vk::Extent2D input_size,
                           vk::Extent2D output_size, float sharpness, bool pq) {
    if (failed || !EmulatorSettings.IsFsr3Enabled()) {
        return input;
    }
    // Like FSR 1: only when the window is larger than the game picture.
    if (input_size.width >= output_size.width && input_size.height >= output_size.height) {
        return input;
    }
    // GT Sport's 10-bit picture (A2R10G10B10) has no FidelityFX format. FSR reads it as
    // A2B10G10R10 (the game image allows other views), which swaps red and blue; the output view
    // swaps them back. Its values are display-encoded (sRGB, or PQ for HDR10), not linear.
    const bool swap_red_blue = input_format == vk::Format::eA2R10G10B10UnormPack32;
    const vk::Format read_format = swap_red_blue ? vk::Format::eA2B10G10R10UnormPack32 : input_format;
    const uint32_t color_flags = pq              ? FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ
                                 : swap_red_blue ? FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB
                                                 : 0u;
    if (ffxApiGetSurfaceFormatVK(static_cast<VkFormat>(read_format)) ==
        FFX_API_SURFACE_FORMAT_UNKNOWN) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_WARNING(Render_Vulkan, "FSR 3.1: the game picture format {} is not supported; "
                        "using FSR 1", vk::to_string(input_format));
        }
        return input;
    }
    if (!Load()) {
        failed = true;
        return input;
    }
    if (!context || input_size != context_input || output_size.width > context_output.width ||
        output_size.height > context_output.height) {
        if (!CreateContext(input_size, output_size)) {
            failed = true;
            return input;
        }
    }
    if (input_size != aux_size) {
        CreateAuxImages(cmdbuf, input_size);
    }
    if (output_size != cur_size || swap_red_blue != cur_swap_red_blue) {
        cur_size = output_size;
        cur_swap_red_blue = swap_red_blue;
        for (auto& img : available_imgs) {
            img.dirty = true;
        }
    }
    auto& img = available_imgs[cur_image];
    cur_image = (cur_image + 1) % available_imgs.size();
    if (img.dirty) {
        CreateImages(img);
    }

    constexpr vk::ImageSubresourceRange range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    // The game picture is already readable by shaders; make it visible to FSR's compute work, and
    // the output writable.
    const vk::MemoryBarrier2 input_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
    };
    const vk::ImageMemoryBarrier2 output_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = img.output_image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &input_barrier,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &output_barrier,
    });

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = static_cast<VkCommandBuffer>(cmdbuf);
    dispatch.color = Resource(input_image, read_format, input_size, FFX_API_RESOURCE_USAGE_READ_ONLY,
                              FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.depth = Resource(depth, vk::Format::eR32Sfloat, input_size,
                              FFX_API_RESOURCE_USAGE_READ_ONLY,
                              FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.motionVectors = Resource(motion_vectors, vk::Format::eR16G16Sfloat, input_size,
                                      FFX_API_RESOURCE_USAGE_READ_ONLY,
                                      FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.output = Resource(img.output_image, vk::Format::eR16G16B16A16Sfloat, output_size,
                               FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.jitterOffset = {0.0f, 0.0f};
    dispatch.motionVectorScale = {1.0f, 1.0f};
    dispatch.renderSize = {input_size.width, input_size.height};
    dispatch.upscaleSize = {output_size.width, output_size.height};
    dispatch.enableSharpening = sharpness >= 0.0f;
    dispatch.sharpness = std::clamp(sharpness, 0.0f, 1.0f);
    dispatch.frameTimeDelta = 1000.0f / 60.0f;
    dispatch.preExposure = 1.0f;
    // No motion vectors: every frame starts a new history.
    dispatch.reset = true;
    dispatch.cameraNear = 0.1f;
    dispatch.cameraFar = 1000.0f;
    dispatch.cameraFovAngleVertical = 1.0f;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    dispatch.flags = color_flags;
    const auto result = g_api.dispatch(&context, &dispatch.header);
    const bool ok = result == FFX_API_RETURN_OK;
    if (!ok) {
        // The output was not written; stop trying and let FSR 1 take over.
        LOG_ERROR(Render_Vulkan, "FSR 3.1: upscaling failed (code {}); using FSR 1", result);
        failed = true;
    }

    const vk::ImageMemoryBarrier2 return_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = img.output_image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &return_barrier,
    });
    // Logged again whenever the game or window size changes.
    static vk::Extent2D logged_in{}, logged_out{};
    if (ok && (input_size != logged_in || output_size != logged_out)) {
        logged_in = input_size;
        logged_out = output_size;
        LOG_INFO(Render_Vulkan, "FSR 3.1: scaling {}x{} to {}x{}", input_size.width,
                 input_size.height, output_size.width, output_size.height);
    }
    return ok ? img.output_image_view.get() : input;
}

void Pass::CreateImages(Img& img) const {
    img.dirty = false;
    img.swap_red_blue = cur_swap_red_blue;
    img.output_image_view.reset();
    img.output_image.Destroy();
    const vk::ImageCreateInfo info{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent{.width = cur_size.width, .height = cur_size.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eStorage,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    img.output_image.Create(info);
    SetObjectName(device, static_cast<vk::Image>(img.output_image), "FSR 3.1 Output Image #{}",
                  img.id);
    img.output_image_view = Check<"create fsr 3.1 output image view">(
        device.createImageViewUnique(vk::ImageViewCreateInfo{
            .image = img.output_image,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .components = img.swap_red_blue
                              ? vk::ComponentMapping{vk::ComponentSwizzle::eB,
                                                     vk::ComponentSwizzle::eG,
                                                     vk::ComponentSwizzle::eR,
                                                     vk::ComponentSwizzle::eA}
                              : vk::ComponentMapping{},
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1,
            },
        }));
}

void Pass::CreateAuxImages(vk::CommandBuffer cmdbuf, vk::Extent2D input_size) {
    // Runs when the game picture first appears; GT Sport keeps one size after that.
    aux_size = input_size;
    motion_vectors.Destroy();
    depth.Destroy();
    const auto make = [&](VideoCore::UniqueImage& image, vk::Format format, const char* name) {
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent{.width = input_size.width, .height = input_size.height, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        SetObjectName(device, static_cast<vk::Image>(image), "FSR 3.1 {}", name);
    };
    make(motion_vectors, vk::Format::eR16G16Sfloat, "Motion Vectors");
    make(depth, vk::Format::eR32Sfloat, "Depth");

    constexpr vk::ImageSubresourceRange range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    std::array<vk::ImageMemoryBarrier2, 2> to_clear{};
    std::array<vk::ImageMemoryBarrier2, 2> to_read{};
    const vk::Image images[] = {motion_vectors, depth};
    for (size_t i = 0; i < 2; ++i) {
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

float UpscaleRatio() {
    static const float ratio = [] {
        std::string value;
        if (const char* env = std::getenv("SHADGT_UPSCALE_RATIO")) {
            value = env;
        } else if (const auto config = Common::RytyGameConfigValue("upscale_ratio")) {
            value = *config;
        }
        std::erase(value, '"');
        const float parsed = value.empty() ? 0.0f : std::strtof(value.c_str(), nullptr);
        return parsed >= 1.0f ? std::min(parsed, 4.0f) : 0.0f;
    }();
    return ratio;
}

bool TemporalRequested() {
    static const bool requested = [] {
        if (const char* env = std::getenv("SHADGT_FSR_TEMPORAL")) {
            return env[0] == '1';
        }
        const auto config = Common::RytyGameConfigValue("fsr_temporal");
        return EmulatorSettings.IsFsrEnabled() && EmulatorSettings.IsFsr3Enabled() &&
               !(config && *config == "false") && !RytyDlss::TemporalRequested();
    }();
    return requested;
}

Temporal::~Temporal() {
    if (context && g_api.destroy_context) {
        g_api.destroy_context(&context, nullptr);
    }
}

bool Temporal::Available() {
    return LoadApi();
}

bool Temporal::Evaluate(vk::PhysicalDevice physical_device, vk::Device device,
                        vk::CommandBuffer cmdbuf, const TemporalFrame& frame) {
    if (!LoadApi()) {
        return false;
    }
    const auto format_of = [](vk::Format format) {
        return ffxApiGetSurfaceFormatVK(static_cast<VkFormat>(format));
    };
    if (format_of(frame.color_format) == FFX_API_SURFACE_FORMAT_UNKNOWN ||
        format_of(frame.depth_format) == FFX_API_SURFACE_FORMAT_UNKNOWN ||
        format_of(frame.motion_format) == FFX_API_SURFACE_FORMAT_UNKNOWN) {
        LOG_WARNING(Render_Vulkan,
                    "FSR-200: scene formats {} / {} / {} are not supported; keeping the game's TAA",
                    vk::to_string(frame.color_format), vk::to_string(frame.depth_format),
                    vk::to_string(frame.motion_format));
        return false;
    }
    if (!context || frame.render_size != context_input || frame.output_size != context_output) {
        if (context) {
            g_api.destroy_context(&context, nullptr);
            context = nullptr;
        }
        ffxCreateBackendVKDesc backend{};
        backend.header.type = BackendDescType();
        backend.vkDevice = static_cast<VkDevice>(device);
        backend.vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical_device);
        backend.vkDeviceProcAddr = &GetDeviceProcAddr;
        ffxOverrideVersion version_override{};
        version_override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        version_override.header.pNext = &backend.header;
        version_override.versionId = SelectedVersion(backend);
        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext = version_override.versionId ? &version_override.header : &backend.header;
        // Like the DLSS pass: display-encoded color, depth not inverted, motion vectors at the
        // render size without jitter.
        desc.flags = 0;
        desc.maxRenderSize = {frame.render_size.width, frame.render_size.height};
        desc.maxUpscaleSize = {frame.output_size.width, frame.output_size.height};
        desc.fpMessage = OnMessage;
        ffxContext new_context{};
        if (const auto result = g_api.create_context(&new_context, &desc.header, nullptr);
            result != FFX_API_RETURN_OK) {
            LOG_ERROR(Render_Vulkan, "FSR-200: creating the temporal upscaler failed (code {})",
                      result);
            return false;
        }
        context = new_context;
        context_input = frame.render_size;
        context_output = frame.output_size;
        ffxQueryGetProviderVersion version{};
        version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        g_api.query(&context, &version.header);
        NoteVersion(version.versionName);
        LOG_INFO(Render_Vulkan, "FSR-200: temporal FSR {} for the scene, {}x{} to {}x{}",
                 version.versionName ? version.versionName : "(unknown version)",
                 frame.render_size.width, frame.render_size.height, frame.output_size.width,
                 frame.output_size.height);
    }

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
    // DIAG-202 (SHADGT_FSR_JITTER=<x sign><y sign>, e.g. "+-"): jitter signs, to compare.
    static const std::array<float, 2> jitter_sign = [] {
        const char* env = std::getenv("SHADGT_FSR_JITTER");
        if (!env || std::strlen(env) < 2) {
            return std::array<float, 2>{1.0f, 1.0f};
        }
        return std::array<float, 2>{env[0] == '-' ? -1.0f : 1.0f, env[1] == '-' ? -1.0f : 1.0f};
    }();
    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = static_cast<VkCommandBuffer>(cmdbuf);
    dispatch.color = Resource(frame.color, frame.color_format, frame.render_size,
                              FFX_API_RESOURCE_USAGE_READ_ONLY,
                              FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    // FSR-205: depth arrives as R32_SFLOAT; a depth format is described as a depth target, so
    // the backend views it with its depth format.
    const bool depth_format = frame.depth_format == vk::Format::eD32Sfloat ||
                              frame.depth_format == vk::Format::eD16Unorm;
    dispatch.depth = Resource(frame.depth, frame.depth_format, frame.render_size,
                              depth_format ? FFX_API_RESOURCE_USAGE_DEPTHTARGET
                                           : FFX_API_RESOURCE_USAGE_READ_ONLY,
                              FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.motionVectors = Resource(frame.motion, frame.motion_format, frame.render_size,
                                      FFX_API_RESOURCE_USAGE_READ_ONLY,
                                      FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.output = Resource(frame.output, frame.output_format, frame.output_size,
                               FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.jitterOffset = {frame.jitter_x * jitter_sign[0], frame.jitter_y * jitter_sign[1]};
    dispatch.motionVectorScale = {frame.motion_scale_x, frame.motion_scale_y};
    dispatch.renderSize = {frame.render_size.width, frame.render_size.height};
    dispatch.upscaleSize = {frame.output_size.width, frame.output_size.height};
    dispatch.enableSharpening = frame.sharpness >= 0.0f;
    dispatch.sharpness = std::clamp(frame.sharpness, 0.0f, 1.0f);
    dispatch.frameTimeDelta = frame.frame_time_ms;
    dispatch.preExposure = 1.0f;
    dispatch.reset = frame.reset;
    dispatch.cameraNear = 0.1f;
    dispatch.cameraFar = 1000.0f;
    dispatch.cameraFovAngleVertical = 1.0f;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    dispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    if (const auto result = g_api.dispatch(&context, &dispatch.header);
        result != FFX_API_RETURN_OK) {
        LOG_ERROR(Render_Vulkan, "FSR-200: temporal upscaling failed (code {})", result);
        return false;
    }
    return true;
}

} // namespace Vulkan::RytyFsr3
