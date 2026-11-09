// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ryGT: AMD FSR 3.1 upscaling through AMD's FidelityFX API (amd_fidelityfx_vk.dll from FidelityFX
// SDK 1.1.4, the newest FSR with a Vulkan backend; MIT, headers in ryty_gpu/ffx_api). The DLL is
// loaded at run time from next to GTSport.exe when the GT Sport config has "fsr_enabled" (and
// "fsr3_enabled", on by default); without the DLL the renderer keeps using FSR 1.
//
// Pass scales the finished game picture, without motion vectors, depth or camera jitter: it runs
// with zero motion vectors, a flat depth image and its history reset every frame, which leaves
// FSR 3.1's spatial upsampling and RCAS sharpening. Temporal (FSR-200) takes GT Sport's scene
// before its TAA instead, with its depth, motion vectors and jitter, through the temporal scene
// capture of the default renderer (GtsDlssPass), when DLSS is not doing it.
//
// FSR-201: GPU.fsr_upscaler picks the upscaler version inside amd_fidelityfx_vk.dll: "fsr3" (the
// newest 3.x, the default) or "fsr4" (the newest 4.x, once AMD's Vulkan DLL has one; until then
// 3.x). SHADGT_FSR_UPSCALER overrides it.

#pragma once

#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan::RytyFsr3 {

/// Scales the game picture into an output image at the window size, like the FSR 1 pass.
class Pass {
public:
    ~Pass();

    void Create(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device,
                VmaAllocator allocator, u32 num_images);

    /// Returns the scaled picture, or input when FSR 3.1 is off, unavailable or not upscaling.
    /// sharpness is FSR 3.1's 0..1 RCAS strength; negative turns sharpening off. pq: the picture
    /// is HDR10 (PQ) encoded.
    vk::ImageView Render(vk::CommandBuffer cmdbuf, vk::Image input_image, vk::ImageView input,
                         vk::Format input_format, vk::Extent2D input_size,
                         vk::Extent2D output_size, float sharpness, bool pq);

private:
    struct Img {
        u8 id{};
        bool dirty{true};
        bool swap_red_blue{}; // the output view swaps red and blue back (see Render)
        VideoCore::UniqueImage output_image;
        vk::UniqueImageView output_image_view;
    };

    bool Load();
    bool CreateContext(vk::Extent2D input_size, vk::Extent2D output_size);
    void DestroyContext();
    void CreateImages(Img& img) const;
    void CreateAuxImages(vk::CommandBuffer cmdbuf, vk::Extent2D input_size);

    vk::PhysicalDevice physical_device{};
    vk::Device device{};
    VmaAllocator allocator{};
    void* context{}; // ffxContext
    vk::Extent2D context_input{};
    vk::Extent2D context_output{};
    vk::Extent2D cur_size{};
    bool cur_swap_red_blue{};
    u32 cur_image{};
    std::vector<Img> available_imgs;
    // Zero motion vectors and a flat depth image at the game's size.
    VideoCore::UniqueImage motion_vectors;
    VideoCore::UniqueImage depth;
    vk::Extent2D aux_size{};
    bool failed{};
};

/// FSR-204: the FSR upscaler created last ("FSR 3.1.4"), for the window title.
std::string ActiveVersionName();

/// FSR-200: temporal FSR of the captured scene (GPU.fsr_temporal, on by default with FSR 3.1;
/// SHADGT_FSR_TEMPORAL=0/1 overrides). Not with DLSS, which has the scene first.
bool TemporalRequested();

/// FSR-209: the temporal upscalers' output as a multiple of the scene's size (GPU.upscale_ratio,
/// the launcher's preset: 1 native anti-aliasing, 1.5 FSR Quality, 2 FSR Performance, ...;
/// SHADGT_UPSCALE_RATIO overrides). 0, the default: the window's size.
float UpscaleRatio();

/// One frame of the captured scene. Images are readable by compute shaders (color, depth, motion)
/// and the output is in the general layout. Motion vectors and jitter follow the DLSS pass.
struct TemporalFrame {
    vk::Image color;
    vk::Format color_format;
    vk::Image depth;
    vk::Format depth_format;
    vk::Image motion;
    vk::Format motion_format;
    vk::Image output;
    vk::Format output_format;
    // XESS-200: XeSS takes image views (FSR makes its own).
    vk::ImageView color_view;
    vk::ImageView depth_view;
    vk::ImageView motion_view;
    vk::ImageView output_view;
    vk::Extent2D render_size;
    vk::Extent2D output_size;
    float jitter_x;
    float jitter_y;
    float motion_scale_x;
    float motion_scale_y;
    float frame_time_ms;
    float sharpness; // RCAS 0..1, negative for none
    bool reset;
};

class Temporal {
public:
    ~Temporal();

    /// Loads amd_fidelityfx_vk.dll; false when it is missing (the scene keeps the game's TAA).
    bool Available();
    /// Records the upscale; false when it failed (the caller stops using FSR for the scene).
    bool Evaluate(vk::PhysicalDevice physical_device, vk::Device device, vk::CommandBuffer cmdbuf,
                  const TemporalFrame& frame);

private:
    void* context{}; // ffxContext
    vk::Extent2D context_input{};
    vk::Extent2D context_output{};
};

/// vkGetDeviceProcAddr for AMD's backend: extension names fall back to core functions, debug
/// labels do nothing (also used by frame generation).
PFN_vkGetDeviceProcAddr DeviceProcAddrShim();

} // namespace Vulkan::RytyFsr3
