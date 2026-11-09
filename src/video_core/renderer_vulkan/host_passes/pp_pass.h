//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Frame;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
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
    };

    void Create(vk::Device device, vk::Format surface_format);

    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings, vk::ImageView ui_layer = {},
                bool dlss_scaled = false, float side_x0 = 0.0f, float side_x1 = 1.0f);

private:
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
};

} // namespace Vulkan::HostPasses
