// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// FIX-SGT-242: the size of GT7's rear-view mirror (the inset of the bumper and bonnet views) in a
// scene that is not 16:9, kept free of the game for the unit tests.
//
// GT7 keeps the mirror as a width and a height in units of a 1920x1080 layout (800 x 128, centred
// on 960), a view angle (20 degrees) and a scale (1). It draws it over width/1920 of the scene's
// width and height/1080 of its height, with a camera of angle scale*angle and of aspect
// width / (scale*height) (read in the code, GT7 1.71). So in a 3840x1080 scene the mirror is
// 1600 pixels wide and in a 1920x1440 one 170 lines high, with the 800:128 camera stretched over
// it. The setting below gives the mirror its 16:9 size in pixels and keeps that camera.

#include <cstdint>
#include "ryty_gpu/gt7_menu_layout.h"

namespace Gt7MirrorSize {

struct Setting {
    std::int32_t width, height; // of a 1920x1080 layout
    float scale;
    float angle; // degrees
};

constexpr Setting Default{800, 128, 1.0f, 20.0f};

/// The setting for a scene of that size: the mirror takes the share of the scene it takes of the
/// 16:9 panel inside it (Gt7MenuLayout::Panel), and scale*angle and width/(scale*height) stay
/// Default's, so the camera is the 16:9 one.
constexpr Setting ForScene(std::uint32_t scene_width, std::uint32_t scene_height) {
    const Gt7MenuLayout::Extent panel = Gt7MenuLayout::Panel(scene_width, scene_height);
    if (!scene_width || !scene_height ||
        (panel.width == scene_width && panel.height == scene_height)) {
        return Default;
    }
    const auto share = [](std::int32_t whole, std::uint32_t part, std::uint32_t of) {
        return static_cast<std::int32_t>((std::uint64_t(whole) * part + of / 2) / of);
    };
    const std::int32_t width = share(Default.width, panel.width, scene_width);
    const std::int32_t height = share(Default.height, panel.height, scene_height);
    const float scale = float(width) * float(Default.height) / (float(Default.width) * float(height));
    return {width, height, scale, Default.angle / scale};
}

} // namespace Gt7MirrorSize
