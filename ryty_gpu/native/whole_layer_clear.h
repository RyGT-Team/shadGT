// Copyright 2026 RyGT team. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Native {
inline bool WholeLayerPatternSafe(std::uint32_t lo, std::uint32_t hi,
                                  std::uint32_t texel_bytes, std::uint64_t layer_stride) {
    if (lo != hi || !texel_bytes || !layer_stride) return false;
    const std::uint32_t byte = lo & 0xff;
    return lo == byte * 0x01010101u || (texel_bytes % 4 == 0 && layer_stride % 4 == 0);
}
// Returns a count only for a prefix of complete, equally sized layers of a
// single-level 2D image and a pattern whose phase is proved safe. Padded layer
// strides can shift patterns, so ambiguous cases keep the existing fallback.
inline std::uint32_t WholeLayerClearCount(std::uint64_t guest_bytes, std::uint64_t written,
                                          std::uint32_t layers, std::uint32_t image_layers,
                                          std::uint32_t levels, std::uint32_t image_levels,
                                          std::uint64_t mip_offset, std::uint64_t mip_bytes,
                                          bool image_2d, bool volume, bool uniform_pattern = true) {
    if (!uniform_pattern || !image_2d || volume || levels != 1 || image_levels != 1 || layers < 2 ||
        image_layers != layers || mip_offset != 0 || mip_bytes != guest_bytes ||
        guest_bytes == 0 || guest_bytes % layers || written == 0 || written >= guest_bytes)
        return 0;
    const auto stride = guest_bytes / layers;
    if (!stride || written % stride) return 0;
    const auto count = written / stride;
    return count < layers ? static_cast<std::uint32_t>(count) : 0;
}
} // namespace Native
