//  SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/pp_pass.h"

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/post_process_frag.h"
#include "video_core/renderer_vulkan/hdr_rolloff_policy.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#ifdef RYTY_GPU
#include "ryty_gpu/ryty_pro_mode.h"
#include "video_core/renderer_vulkan/vk_ui_hires.h"

namespace Vulkan {
float RytyDisplayAspect();
}
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <boost/container/static_vector.hpp>
#include <cstring>
#include <vk_mem_alloc.h>

namespace Vulkan::HostPasses {

namespace {
struct PostProcessingConstants {
    PostProcessingPass::Settings settings;
    float dlss_sharpening = 0.0f;
    // WIDE-042: the ambient sides (outside side_x0..side_x1 of the width) dimmed by side_dim.
    float side_x0 = 0.0f;
    float side_x1 = 1.0f;
    float side_dim = 1.0f;
};
// Must match the push-constant block in post_process.frag.
static_assert(sizeof(PostProcessingPass::Settings) == 56);
static_assert(offsetof(PostProcessingConstants, dlss_sharpening) == 56);
static_assert(sizeof(PostProcessingConstants) == 72);
} // namespace

void PostProcessingPass::Create(vk::Device device, vk::PhysicalDevice physical_device) {
    boost::container::static_vector<vk::DescriptorSetLayoutBinding, 5> bindings{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
        {
            .binding = 1, // ryGT HDR-UI: the UI layer
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
        { // GT7-UW: the HUD over black
            .binding = 2,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
        { // GT7-UW: the HUD over white
            .binding = 3,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
        { // GT7-UW: the HUD layout
            .binding = 4,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
    };

    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci{
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };

    desc_set_layout = Check<"create pp descriptor set layout">(
        device.createDescriptorSetLayoutUnique(desc_layout_ci));

    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(PostProcessingConstants),
    };

    const auto& vs_module = CompileSPV(FS_TRI_VERT, device);
    ASSERT(vs_module);
    SetObjectName(device, vs_module, "fs_tri.vert");

    const auto& fs_module = CompileSPV(POST_PROCESS_FRAG, device);
    ASSERT(fs_module);
    SetObjectName(device, fs_module, "post_process.frag");

    const std::array shaders_ci{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vs_module,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fs_module,
            .pName = "main",
        },
    };

    const vk::PipelineLayoutCreateInfo layout_info{
        .setLayoutCount = 1U,
        .pSetLayouts = &*desc_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };

    pipeline_layout =
        Check<"create pp pipeline layout">(device.createPipelineLayoutUnique(layout_info));

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{
        .vertexBindingDescriptionCount = 0u,
        .vertexAttributeDescriptionCount = 0u,
    };

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };

    const vk::Viewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = 1.0f,
        .height = 1.0f,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {1, 1},
    };

    const vk::PipelineViewportStateCreateInfo viewport_info{
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    const vk::PipelineRasterizationStateCreateInfo raster_state{
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eBack,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };

    const std::array attachments{
        vk::PipelineColorBlendAttachmentState{
            .blendEnable = false,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        },
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .logicOpEnable = false,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = attachments.size(),
        .pAttachments = attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    const std::array dynamic_states{
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };

    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = dynamic_states.size(),
        .pDynamicStates = dynamic_states.data(),
    };

    vk::PipelineRenderingCreateInfo rendering_info{.colorAttachmentCount = 1};
    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .pNext = &rendering_info,
        .stageCount = shaders_ci.size(),
        .pStages = shaders_ci.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
    };

    // A replacement surface can choose the other 8-bit format while older frames are in flight.
    for (auto& output : output_pipelines) {
        const auto features = physical_device.getFormatProperties(output.format).optimalTilingFeatures;
        if (!(features & vk::FormatFeatureFlagBits::eColorAttachment)) {
            continue;
        }
        rendering_info.pColorAttachmentFormats = &output.format;
        output.pipeline = Check<"create post process pipeline">(
            device.createGraphicsPipelineUnique(/*pipeline_cache*/ {}, pipeline_info));
    }

    // Once pipeline is compiled, we don't need the shader module anymore
    device.destroyShaderModule(vs_module);
    device.destroyShaderModule(fs_module);

    // Create sampler resource
    const vk::SamplerCreateInfo sampler_ci{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
    };
    sampler = Check<"create pp sampler">(device.createSamplerUnique(sampler_ci));
}

void PostProcessingPass::CreateHudBuffer(VmaAllocator allocator) {
    hud_allocator = allocator;
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = HudSlots * HudSlotSize,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VkBuffer buffer{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator, &buffer_ci, &alloc_ci, &buffer, &hud_allocation, &info) ==
        VK_SUCCESS) {
        hud_buffer = buffer;
        hud_mapped = static_cast<u8*>(info.pMappedData);
        std::memset(hud_mapped, 0, HudSlots * HudSlotSize);
    }
}

void PostProcessingPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView input,
                                vk::Extent2D input_size, Frame& frame, Settings settings,
                                vk::ImageView ui_layer, bool dlss_scaled, float side_x0,
                                float side_x1, vk::ImageView hud_black, vk::ImageView hud_white,
                                std::span<const HudWidget> hud_widgets,
                                std::optional<vk::Rect2D> content_rect) {
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Host/Post processing",
        });
    }

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const std::array<vk::RenderingAttachmentInfo, 1> attachments{{
        {
            .imageView = frame.image_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
        },
    }};
    const vk::RenderingInfo rendering_info{
        .renderArea{
            .extent{
                .width = frame.width,
                .height = frame.height,
            },
        },
        .layerCount = 1,
        .colorAttachmentCount = attachments.size(),
        .pColorAttachments = attachments.data(),
    };

    vk::DescriptorImageInfo image_info{
        .sampler = *sampler,
        .imageView = input,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    // GT7-UW: the HUD pictures stay in the general layout native mode keeps its pictures in;
    // without a HUD the input stands in (the shader does not read it then).
    const bool hud = hud_black && hud_white;
    settings.hud = hud ? 1u : 0u;
    const vk::DescriptorImageInfo hud_black_info{
        .sampler = *sampler,
        .imageView = hud ? hud_black : input,
        .imageLayout = hud ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::DescriptorImageInfo hud_white_info{
        .sampler = *sampler,
        .imageView = hud ? hud_white : input,
        .imageLayout = hud ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal,
    };

    // GT7-UW: the layout in this frame's slot of the buffer (a count, then the pieces).
    const u32 slot = hud_slot++ % HudSlots;
    if (hud_mapped) {
        u8* data = hud_mapped + slot * HudSlotSize;
        const u32 count = static_cast<u32>(std::min<size_t>(hud_widgets.size(), MaxHudWidgets));
        const std::array<u32, 4> header{hud ? count : 0u, 0, 0, 0};
        std::memcpy(data, header.data(), sizeof(header));
        if (count != 0) {
            std::memcpy(data + sizeof(header), hud_widgets.data(), count * sizeof(HudWidget));
        }
        vmaFlushAllocation(hud_allocator, hud_allocation, slot * HudSlotSize, HudSlotSize);
    }
    const vk::DescriptorBufferInfo hud_layout_info{
        .buffer = hud_buffer,
        .offset = slot * HudSlotSize,
        .range = 16 + MaxHudWidgets * sizeof(HudWidget),
    };

    vk::DescriptorImageInfo ui_info{
        .sampler = *sampler,
        .imageView = ui_layer ? ui_layer : input,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    settings.ui_layer = ui_layer ? 1 : 0;
    const std::array set_writes{
        vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_info,
        },
        vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &ui_info,
        },
        vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &hud_black_info,
        },
        vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 3,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &hud_white_info,
        },
        vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 4,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &hud_layout_info,
        },
    };

    const auto output = std::ranges::find(output_pipelines, frame.image_view_format,
                                          &OutputPipeline::format);
    if (output == output_pipelines.end() || !output->pipeline) [[unlikely]] {
        UNREACHABLE_MSG("Unsupported post-processing attachment format: {}",
                        vk::to_string(frame.image_view_format));
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *output->pipeline);

    // With a widened camera the guest can still submit a 16:9 buffer. Its projection
    // expects the full window. GT Sport's True wide compositor fits its boot screens.
    vk::Rect2D area = content_rect.value_or(vk::Rect2D{{0, 0}, {frame.width, frame.height}});
#ifdef RYTY_GPU
    static const bool fit_narrow_input =
        RytyDisplayAspect() == 0.0f || (!Common::IsGt7() && UiHires::Wide());
#else
    constexpr bool fit_narrow_input = true;
#endif
    if (!content_rect && fit_narrow_input && input_size.width && input_size.height && frame.height) {
        const double input_aspect = double(input_size.width) / input_size.height;
        const double frame_aspect = double(frame.width) / frame.height;
        if (input_aspect < frame_aspect * 0.8) {
            const u32 width = u32(std::lround(frame.height * input_aspect));
            area = vk::Rect2D{{s32((frame.width - width) / 2), 0}, {width, frame.height}};
        }
    }
#ifdef RYTY_GPU
    static thread_local vk::Extent2D logged_extent{};
    if (!fit_narrow_input && frame.width * 9u > frame.height * 16u &&
        area.extent != logged_extent) {
        LOG_INFO(Render_Vulkan, "Ultrawide viewport: input {}x{}, output {}x{}, scene {}x{} at x={}",
                 input_size.width, input_size.height, frame.width, frame.height,
                 area.extent.width, area.extent.height, area.offset.x);
        logged_extent = area.extent;
    }
#endif
    const std::array viewports = {
        vk::Viewport{
            .x = static_cast<float>(area.offset.x),
            .y = static_cast<float>(area.offset.y),
            .width = static_cast<float>(area.extent.width),
            .height = static_cast<float>(area.extent.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        },
    };

    cmdbuf.setViewport(0, viewports);
    cmdbuf.setScissor(0, area);

    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, set_writes);
    float dlss_sharpening = 0.0f;
#ifdef RYTY_GPU
    {
        // The HDR settings are read again every 2 seconds, so they can be tuned while playing.
        static const char* const peak_override = std::getenv("SHADGT_HDR_ROLLOFF_NITS");
        static float peak_nits = HdrRolloffDefaultNits, gain = 1.0f, sdr_white = 200.0f;
        static bool input_709 = false;
        static float ui_white = 250.0f;
        static float saturation = 1.0f;
        static float sharpening = 0.0f;
        static auto next_read = std::chrono::steady_clock::time_point{};
        if (const auto now = std::chrono::steady_clock::now(); now >= next_read) {
            next_read = now + std::chrono::seconds(2);
            // FIX-SGT-113: without the key, no roll-off.
            const double configured_peak =
                Ryty::ConfigNumber("hdr_peak_nits", HdrRolloffDefaultNits);
            peak_nits = HdrRolloffNits(peak_override, static_cast<float>(configured_peak));
            gain = static_cast<float>(Ryty::ConfigNumber("hdr_brightness", 100.0) / 100.0);
            sdr_white = static_cast<float>(Ryty::ConfigNumber("hdr_sdr_white_nits", 200.0));
            input_709 = Ryty::ConfigTrue("hdr_input_709");
            ui_white = static_cast<float>(Ryty::ConfigNumber("hdr_ui_white_nits", 250.0));
            saturation = static_cast<float>(Ryty::ConfigNumber("hdr_saturation", 1.0));
            const double configured_sharpening = Ryty::ConfigNumber("dlss_sharpening", 0.0);
            sharpening = std::isfinite(configured_sharpening)
                             ? static_cast<float>(std::clamp(configured_sharpening, 0.0, 100.0) /
                                                  100.0)
                             : 0.0f;
        }
        settings.hdr_peak_nits = peak_nits;
        settings.hdr_gain = gain;
        settings.sdr_white_nits = sdr_white;
        settings.hdr_input_709 = input_709 ? 1 : 0;
        settings.ui_white_nits = ui_white;
        settings.hdr_saturation = saturation;
        // The swapchain is HDR10 whenever HDR is allowed; the game's frame may still be SDR.
        settings.sdr_input = frame.is_hdr && !settings.hdr ? 1 : 0;
        settings.hdr = frame.is_hdr ? 1 : 0;
        dlss_sharpening = dlss_scaled ? sharpening : 0.0f;
    }
#endif
    float side_dim = 1.0f;
#ifdef RYTY_GPU
    if (side_x0 > 0.0f || side_x1 < 1.0f) {
        // WIDE-042: GPU.ambient_brightness in percent (default 45).
        static float brightness = 0.45f;
        static auto next = std::chrono::steady_clock::time_point{};
        if (const auto now = std::chrono::steady_clock::now(); now >= next) {
            next = now + std::chrono::seconds(2);
            const double value = Ryty::ConfigNumber("ambient_brightness", 45.0);
            brightness = std::isfinite(value) ? float(std::clamp(value, 0.0, 100.0) / 100.0) : 0.45f;
        }
        side_dim = brightness;
    }
#endif
    const PostProcessingConstants constants{settings, dlss_sharpening, side_x0, side_x1, side_dim};
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0,
                         sizeof(constants), &constants);

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.draw(3, 1, 0, 0);
    cmdbuf.endRendering();

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame.image,
        .subresourceRange = simple_subresource,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
}

} // namespace Vulkan::HostPasses
