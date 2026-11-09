// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace VideoCore {
constexpr std::uint64_t NativePackedTilingKey(std::uint64_t base, bool packed, bool tiled) {
    return base | (std::uint64_t(packed) << 50) | (std::uint64_t(!tiled) << 51);
}
constexpr std::uint64_t NativeTilingGroups(std::uint64_t bytes, std::uint32_t texel_bytes) {
    if (!texel_bytes) return 0;
    const auto elements = bytes / texel_bytes;
    return elements / 64 + (elements % 64 != 0);
}
// Rearrange raw 10/11/11 bit fields without converting their floating values.
constexpr std::uint32_t PackedFloatToVulkan(std::uint32_t value) {
    return (value >> 21) | (((value >> 10) & 0x7ffu) << 11) |
           ((value & 0x3ffu) << 22);
}
constexpr std::uint32_t PackedFloatFromVulkan(std::uint32_t value) {
    return (value >> 22) | (((value >> 11) & 0x7ffu) << 10) |
           ((value & 0x7ffu) << 21);
}
}
