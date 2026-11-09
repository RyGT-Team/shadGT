// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace Native {
constexpr std::uint32_t CubeViewLayers(bool supported, bool volume,
                                       std::uint32_t width, std::uint32_t height,
                                       std::uint32_t image_layers, std::uint32_t samples,
                                       std::uint32_t requested_layers) {
    return supported && !volume && width == height && image_layers >= 6 && samples == 1
               ? (requested_layers / 6) * 6 : 0;
}
}
