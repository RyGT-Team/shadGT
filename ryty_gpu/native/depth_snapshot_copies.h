// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Native {
inline std::vector<vk::ImageCopy> DepthSnapshotCopies(std::uint32_t width, std::uint32_t height,
                                                       std::uint32_t mips, std::uint32_t layers, bool stencil) {
    std::vector<vk::ImageCopy> copies;
    copies.reserve(mips * (stencil ? 2u : 1u));
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        for (auto aspect : {vk::ImageAspectFlagBits::eDepth, vk::ImageAspectFlagBits::eStencil}) {
            if (aspect == vk::ImageAspectFlagBits::eStencil && !stencil) continue;
            vk::ImageCopy copy{};
            copy.srcSubresource.aspectMask = aspect;
            copy.srcSubresource.mipLevel = mip;
            copy.srcSubresource.layerCount = layers;
            copy.dstSubresource = copy.srcSubresource;
            copy.extent.width = std::max(width >> mip, 1u);
            copy.extent.height = std::max(height >> mip, 1u);
            copy.extent.depth = 1;
            copies.push_back(copy);
        }
    }
    return copies;
}
} // namespace Native
