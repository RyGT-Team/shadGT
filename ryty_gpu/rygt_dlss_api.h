// SPDX-License-Identifier: MIT
//
// The interface between ryGT's renderer (libSceVideoOut.prx) and the optional DLSS plugin
// (dlss\ryGT_dlss.dll next to GTSport.exe). The plugin links NVIDIA's DLSS SDK, whose license does
// not allow it inside the GPL renderer, so the renderer only loads the plugin at run time when the
// DLSS option is on and the plugin is present. Plain C; Vulkan handles are passed as is.
//

#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RYGT_DLSS_API_VERSION 1
#define RYGT_DLSS_TEMPORAL_API_VERSION 2

typedef struct RygtDlssImage {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width;
    uint32_t height;
} RygtDlssImage;

// Returns RYGT_DLSS_API_VERSION.
typedef int(VKAPI_PTR* PFN_RygtDlss_ApiVersion)(void);

// Instance extensions DLSS needs; call before creating the VkInstance. The list stays valid until
// the plugin is unloaded. Returns 0 on success.
typedef int(VKAPI_PTR* PFN_RygtDlss_GetInstanceExtensions)(uint32_t* count, const char* const** names);

// Device extensions DLSS needs on this physical device; call before creating the VkDevice.
typedef int(VKAPI_PTR* PFN_RygtDlss_GetDeviceExtensions)(VkInstance instance,
                                                         VkPhysicalDevice physical_device,
                                                         PFN_vkGetInstanceProcAddr gipa,
                                                         uint32_t* count, const char* const** names);

// Starts DLSS on the device. data_path receives NVIDIA's logs. Returns 0 when DLSS is available.
typedef int(VKAPI_PTR* PFN_RygtDlss_Init)(VkInstance instance, VkPhysicalDevice physical_device,
                                          VkDevice device, PFN_vkGetInstanceProcAddr gipa,
                                          PFN_vkGetDeviceProcAddr gdpa, const wchar_t* data_path);

// Whether DLSS can scale input_width x input_height to output_width x output_height (an output at
// least as large as the input, within the range one of DLSS's quality modes accepts).
typedef int(VKAPI_PTR* PFN_RygtDlss_Supports)(uint32_t input_width, uint32_t input_height,
                                              uint32_t output_width, uint32_t output_height);

// Records DLSS into cmd: input (VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) scaled into output
// (VK_IMAGE_LAYOUT_GENERAL, created with VK_IMAGE_USAGE_STORAGE_BIT). sharpness is 0..1.
// Returns 0 on success; on failure nothing usable was written to output.
typedef int(VKAPI_PTR* PFN_RygtDlss_Evaluate)(VkCommandBuffer cmd, const RygtDlssImage* input,
                                              const RygtDlssImage* output, float sharpness);

// Releases DLSS. The device must be idle.
typedef void(VKAPI_PTR* PFN_RygtDlss_Shutdown)(void);

// The last error, for the log.
typedef const char*(VKAPI_PTR* PFN_RygtDlss_LastError)(void);

typedef enum RygtDlssQuality {
    RYGT_DLSS_QUALITY_DLAA = 0,
    RYGT_DLSS_QUALITY_QUALITY = 1,
    RYGT_DLSS_QUALITY_BALANCED = 2,
    RYGT_DLSS_QUALITY_PERFORMANCE = 3,
    RYGT_DLSS_QUALITY_ULTRA_PERFORMANCE = 4
} RygtDlssQuality;

typedef enum RygtDlssFrameFlags {
    RYGT_DLSS_FRAME_HDR = 1u << 0,
    RYGT_DLSS_FRAME_DEPTH_INVERTED = 1u << 1,
    RYGT_DLSS_FRAME_MOTION_JITTERED = 1u << 2,
    RYGT_DLSS_FRAME_MOTION_OUTPUT_RESOLUTION = 1u << 3,
    RYGT_DLSS_FRAME_RESET = 1u << 4,
    RYGT_DLSS_FRAME_AUTO_EXPOSURE = 1u << 5
} RygtDlssFrameFlags;

typedef struct RygtDlssOptimalSettings {
    uint32_t struct_size;
    uint32_t optimal_width, optimal_height;
    uint32_t minimum_width, minimum_height;
    uint32_t maximum_width, maximum_height;
} RygtDlssOptimalSettings;

typedef struct RygtDlssFrame {
    uint32_t struct_size;
    uint32_t version;
    uint64_t frame_id;
    RygtDlssImage color;
    RygtDlssImage depth;
    RygtDlssImage motion;
    RygtDlssImage output;
    RygtDlssQuality quality;
    uint32_t flags;
    float jitter_x, jitter_y;
    float motion_scale_x, motion_scale_y;
    float pre_exposure, exposure_scale;
    float frame_time_ms;
} RygtDlssFrame;

typedef int(VKAPI_PTR* PFN_RygtDlss_TemporalApiVersion)(void);
typedef int(VKAPI_PTR* PFN_RygtDlss_GetOptimalSettings)(
    uint32_t outputWidth, uint32_t outputHeight, RygtDlssQuality quality,
    RygtDlssOptimalSettings* settings);

typedef int(VKAPI_PTR* PFN_RygtDlss_EvaluateTemporal)(VkCommandBuffer cmd,
                                                   const RygtDlssFrame* frame);

#ifdef __cplusplus
}
#endif
