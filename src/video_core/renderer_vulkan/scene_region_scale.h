// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace Vulkan {
struct SceneRegionExtent { std::uint32_t width{}, height{}; };
// Match the game's positive float-to-int viewport conversion, including truncation.
constexpr SceneRegionExtent ScaleSceneRegion(std::uint32_t width, std::uint32_t height,
                                            std::uint32_t scene_width, std::uint32_t scene_height) {
    if (!width || !height || width > 1920 || height > 1080 ||
        scene_width < 320 || scene_width > 7680 || scene_height < 180 || scene_height > 2160)
        return {};
    return {std::uint32_t(float(width) * (float(scene_width) / 1920.f)),
            std::uint32_t(float(height) * (float(scene_height) / 1080.f))};
}
} // namespace Vulkan
