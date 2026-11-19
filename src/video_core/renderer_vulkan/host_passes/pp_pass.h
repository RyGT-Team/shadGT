//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;

namespace Vulkan {
class Frame;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
    /// GT7-UW: a HUD piece moved off its place: where it is in the HUD picture (x, y, w, h,
    /// 0..1 of the 16:9 HUD) and where it goes on the output (0..1 of the output).
    struct HudWidget {
        float src[4];
        float dst[4];
    };
    static constexpr u32 MaxHudWidgets = 16;

    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
        u32 srgb_input = 0;
        // ryGT: HDR output (PQ). The display's peak in nits (0: no roll-off) and a brightness
        // scale applied before it (GPU.hdr_peak_nits, GPU.hdr_brightness in percent).
        float hdr_peak_nits = 0.0f;
        float hdr_gain = 1.0f;
        // ryGT: an SDR frame shown on the HDR10 swapchain, and its white in nits.
        u32 sdr_input = 0;
        float sdr_white_nits = 200.0f;
        // ryGT: the game's HDR frame holds BT.709 colours (converted to BT.2020 here).
        u32 hdr_input_709 = 0;
        // ryGT HDR-UI: a UI layer (binding 1) is composed over the scene at this white level.
        u32 ui_layer = 0;
        float ui_white_nits = 250.0f;
        float hdr_saturation = 1.0f;
        /// GT7-UW: the HUD drawn over black and white is laid over the picture between these
        /// horizontal positions (0..1 of the output).
        u32 hud = 0;
        float hud_x0 = 0.0f;
        float hud_x1 = 1.0f;
    };

    /// FIX-GT7-145: the format of a frame shown on an HDR10 output, the swapchain's own 10 bits.
    /// The surface's 8-bit format cut the PQ picture to 256 levels a channel before the 10-bit
    /// swapchain (measured: 85 to 94% of the window's pixels on the 8-bit grid, 22 to 25% in
    /// the game's picture, which is chance).
    static constexpr vk::Format HdrFrameFormat = vk::Format::eA2B10G10R10UnormPack32;

    void Create(vk::Device device, vk::Format surface_format);
    /// GT7-UW: the buffer the HUD layout goes to the shader in.
    void CreateHudBuffer(VmaAllocator allocator);

    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings, vk::ImageView ui_layer = {},
                bool dlss_scaled = false, float side_x0 = 0.0f, float side_x1 = 1.0f,
                vk::ImageView hud_black = {}, vk::ImageView hud_white = {},
                std::span<const HudWidget> hud_widgets = {});

private:
    vk::UniquePipeline pipeline{};
    vk::UniquePipeline hdr_pipeline{}; // FIX-GT7-145: for HdrFrameFormat
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
    // GT7-UW: the HUD layout, one slot per recent frame.
    static constexpr u32 HudSlots = 8;
    static constexpr u32 HudSlotSize = 1024;
    VmaAllocator hud_allocator{};
    vk::Buffer hud_buffer{};
    VmaAllocation hud_allocation{};
    u8* hud_mapped = nullptr;
    u32 hud_slot = 0;
};

} // namespace Vulkan::HostPasses
