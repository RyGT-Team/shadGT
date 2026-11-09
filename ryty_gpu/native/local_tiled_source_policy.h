// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace Native {
constexpr bool PreferLocalTiledSource(std::uint64_t size, bool tiled,
                                      std::uint32_t vendor, unsigned kind,
                                      std::uint64_t offset, std::uint64_t capacity) {
    return vendor == 0x10de && tiled && (kind == 2 || kind == 4) &&
           size >= (4ull << 20) && size <= (192ull << 20) &&
           (size & 3) == 0 && (offset & 3) == 0 &&
           offset <= capacity && size <= capacity - offset;
}
}
