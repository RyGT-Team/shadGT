// Copyright 2026 RyGT team. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>

namespace Vulkan {
// unset_mb: the size without the switch (TITLE-001: 64 MB for GT Sport, PERF-GTS-407; 1 MB for
// GT7, FIX-GT7-047).
inline std::uint64_t NativeChunkBytes(const char* value, std::uint64_t unset_mb = 64) {
    constexpr std::uint64_t mb = 1024 * 1024;
    if (!value || !*value) return unset_mb * mb;
    std::uint64_t count{};
    const auto end = value + std::strlen(value);
    const auto result = std::from_chars(value, end, count);
    return result.ec == std::errc{} && result.ptr == end && count >= 1 && count <= 64
               ? count * mb : mb;
}
inline std::uint64_t NativeRegionUploadStep(std::uint64_t remaining, std::uint64_t chunk) {
    return (std::min)({std::uint64_t{4} * 1024 * 1024, chunk, remaining});
}
} // namespace Vulkan
