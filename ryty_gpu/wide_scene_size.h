// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// WIDE-001's scene size at the screen's shape (RytySceneSize), kept free of the game for the unit
// tests. FEAT-SGT-210: on a screen taller than 16:9 the width is kept and the height grows.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Vulkan::WideScene {

struct Extent {
    std::uint32_t width = 0, height = 0;
};

constexpr double MaxPixels = 3840.0 * 2160.0; // what the scene scale allows at 16:9

/// The scene for a screen of `aspect` (width over height), from `base` (1920x1080, or the
/// launcher's scene scale). The height is `wanted_height` (GPU.wide_scene_height) when it is
/// within 360..2160; else, on a taller screen (`tall`, FEAT-SGT-210), base's width over the
/// aspect, rounded down to 8 (1920 -> 1200 at 16:10, 1440 at 4:3); else base's height. The width
/// follows from the height and the aspect, in steps of 16 and at most 7680, within MaxPixels.
inline Extent Size(Extent base, double aspect, std::uint32_t wanted_height, bool tall) {
    if (!(aspect > 0.0) || base.width == 0 || base.height == 0) {
        return base;
    }
    std::uint32_t height = base.height;
    if (wanted_height >= 360 && wanted_height <= 2160) {
        height = wanted_height;
    } else if (tall) {
        height = std::uint32_t(std::lround(double(base.width) / aspect)) & ~7u;
    }
    if (double(height) * height * aspect > MaxPixels) {
        height = std::uint32_t(std::sqrt(MaxPixels / aspect)) & ~7u;
    }
    const std::uint32_t width =
        std::min<std::uint32_t>(std::uint32_t(std::lround(height * aspect / 16.0)) * 16, 7680);
    return Extent{width, height};
}

} // namespace Vulkan::WideScene
