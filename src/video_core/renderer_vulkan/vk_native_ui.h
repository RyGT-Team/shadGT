// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_vulkan/vk_ui_hires.h"

namespace Vulkan::UiHires {

inline void NativeBarrier(vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout from,
                          vk::ImageLayout to) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = ColorRange,
    };
    cmd.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

inline vk::ImageBlit2 NativeBlit(vk::Extent2D from, vk::Extent2D to) {
    return vk::ImageBlit2{
        .srcSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .srcOffsets =
            std::array{vk::Offset3D{}, vk::Offset3D{s32(from.width), s32(from.height), 1}},
        .dstSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .dstOffsets = std::array{vk::Offset3D{}, vk::Offset3D{s32(to.width), s32(to.height), 1}},
    };
}

inline void UpscaleNative(vk::CommandBuffer cmd, vk::Image image, vk::ImageView view,
                          vk::Format format, Proxy& proxy) {
    proxy.full = false;
    const bool dlss =
        !Wide() && !proxy.dlss_failed && RytyDlss::CanScale(proxy.native, proxy.size);
    const auto input_layout =
        dlss ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eTransferSrcOptimal;
    NativeBarrier(cmd, image, vk::ImageLayout::eGeneral, input_layout);
    const bool scaled = dlss && RytyDlss::ScaleInto(cmd, image, view, format, proxy.native,
                                                    proxy.scaled, *proxy.scaled_view, proxy.size);
    if (dlss && !scaled) {
        proxy.dlss_failed = true;
        NativeBarrier(cmd, image, input_layout, vk::ImageLayout::eTransferSrcOptimal);
    }
    proxy.dlss_used = scaled;
    if (Wide()) {
        ClearProxyForBlit(cmd, proxy); // WIDE-001: black beside the 16:9 picture
    } else {
        ProxyBarrier(cmd, proxy, vk::ImageLayout::eTransferDstOptimal,
                     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    }
    const auto region = IntoProxy(proxy, scaled ? proxy.size : proxy.native);
    if (scaled) {
        NativeBarrier(cmd, proxy.scaled, vk::ImageLayout::eGeneral,
                      vk::ImageLayout::eTransferSrcOptimal);
    }
    cmd.blitImage2(vk::BlitImageInfo2{
        .srcImage = scaled ? vk::Image{proxy.scaled} : image,
        .srcImageLayout = vk::ImageLayout::eTransferSrcOptimal,
        .dstImage = proxy.image,
        .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
        .regionCount = 1,
        .pRegions = &region,
        .filter = scaled ? vk::Filter::eNearest : vk::Filter::eLinear,
    });
    NativeBarrier(cmd, image, scaled ? input_layout : vk::ImageLayout::eTransferSrcOptimal,
                  vk::ImageLayout::eGeneral);
    ProxyBarrier(cmd, proxy, vk::ImageLayout::eGeneral, DrawStages, DrawAccess);
    ClearUiLayer(cmd, proxy);
    proxy.phase = Phase::Ui;
}

inline void ResolveNative(vk::CommandBuffer cmd, vk::Image image, Proxy& proxy, bool keep) {
    if (proxy.native_stale) {
        ProxyBarrier(cmd, proxy, vk::ImageLayout::eTransferSrcOptimal,
                     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        NativeBarrier(cmd, image, vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferDstOptimal);
        const auto regions = FromProxySpread(proxy, proxy.native); // WIDE-001/005
        cmd.blitImage2(vk::BlitImageInfo2{
            .srcImage = proxy.image,
            .srcImageLayout = vk::ImageLayout::eTransferSrcOptimal,
            .dstImage = image,
            .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
            .regionCount = u32(regions.size()),
            .pRegions = regions.data(),
            .filter = vk::Filter::eLinear,
        });
        NativeBarrier(cmd, image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral);
        ProxyBarrier(cmd, proxy, vk::ImageLayout::eGeneral, DrawStages, DrawAccess);
        MarkStale(proxy, false);
    }
    if (!keep) {
        proxy.phase = Phase::Off;
    }
}

} // namespace Vulkan::UiHires
