// SPDX-License-Identifier: GPL-2.0-or-later

// The geometry of the game's picture inside an output of another shape (UiHires, WIDE-001 and
// FEAT-SGT-203), kept free of Vulkan for the unit tests.

#pragma once

#include <cmath>
#include <cstdint>

namespace Vulkan::UiFit {

struct Rect {
    std::int32_t x = 0, y = 0;
    std::uint32_t width = 0, height = 0;
};

/// The rectangle of the picture's shape (`from_w` x `from_h`) centred in the output, as large as
/// fits: the full height between bars at the sides on a wider output, the full width between
/// bands at the top and bottom on a taller one (FEAT-SGT-203).
inline Rect Centred(std::uint32_t out_w, std::uint32_t out_h, std::uint32_t from_w,
                    std::uint32_t from_h) {
    if (from_w == 0 || from_h == 0) {
        return Rect{0, 0, out_w, out_h};
    }
    auto width = std::uint32_t(std::lround(double(out_h) * from_w / from_h));
    std::uint32_t height = out_h;
    if (width > out_w) {
        width = out_w;
        height = std::uint32_t(std::lround(double(out_w) * from_h / from_w));
    }
    return Rect{std::int32_t((out_w - width) / 2), std::int32_t((out_h - height) / 2), width,
                height};
}

/// The output is at least the picture's size both ways.
inline bool Covers(std::uint32_t out_w, std::uint32_t out_h, std::uint32_t from_w,
                   std::uint32_t from_h) {
    return out_w >= from_w && out_h >= from_h;
}

/// The output's shape is wider than the picture's.
inline bool Wider(std::uint32_t out_w, std::uint32_t out_h, std::uint32_t from_w,
                  std::uint32_t from_h) {
    return std::int64_t(out_w) * from_h > std::int64_t(out_h) * from_w;
}

/// The output's shape is taller than the picture's (FEAT-SGT-203: 16:10 or 4:3 for 16:9).
inline bool Taller(std::uint32_t out_w, std::uint32_t out_h, std::uint32_t from_w,
                   std::uint32_t from_h) {
    return std::int64_t(out_w) * from_h < std::int64_t(out_h) * from_w;
}

} // namespace Vulkan::UiFit
