// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The 16:9 panel GT7's menus are rendered at inside a display of another shape (Gt7NativeMenu),
// kept free of the game for the unit tests. FEAT-SGT-243: also a display taller than 16:9.

#include <algorithm>
#include <cstdint>

namespace Gt7MenuLayout {

struct Extent {
    std::uint32_t width = 0, height = 0;
};

/// The largest 16:9 extent inside the display: the display's height on one wider than 16:9
/// (2560x1440 in 3440x1440), its width on a taller one (1920x1080 in 1920x1200).
constexpr Extent Panel(std::uint32_t display_width, std::uint32_t display_height) {
    return {std::min(display_width, (display_height * 16 + 4) / 9),
            std::min(display_height, (display_width * 9 + 8) / 16)};
}

/// Whether the display is not 16:9, so that the menus and the track differ in size.
constexpr bool Shaped(std::uint32_t display_width, std::uint32_t display_height) {
    const Extent panel = Panel(display_width, display_height);
    return panel.width < display_width || panel.height < display_height;
}

} // namespace Gt7MenuLayout
