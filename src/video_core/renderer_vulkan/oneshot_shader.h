// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Vulkan {
// Source GT7 thumbnail passes are drawn only once; missing either shader can
// permanently omit the saved color image. Other shader hashes remain unchanged.
constexpr bool IsGt7OneShotShader(std::uint64_t hash) {
    return hash == 0x9b9bf2a4ull || hash == 0x40327433ull;
}
}
