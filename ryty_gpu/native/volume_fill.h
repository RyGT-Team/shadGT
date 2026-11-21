// Copyright 2026 RyGT team. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>

namespace Native {

struct VolumeFillRegion {
    std::uint64_t offset;
    std::uint32_t z, y, width, height, depth;
    bool operator==(const VolumeFillRegion&) const = default;
};

struct VolumeFillPlan {
    std::array<VolumeFillRegion, 3> regions{};
    unsigned count{};
    std::uint64_t buffer_bytes{}; // exclusive end of the last visible texel the GPU reads
};

// Decompose a guest byte prefix into complete slices, complete rows and complete texels.
// Using guest offsets and pitch for each copy preserves the phase of the alternating fill.
inline std::optional<VolumeFillPlan> PlanLinearVolumeFill(
    std::uint32_t width, std::uint32_t height, std::uint32_t depth, std::uint32_t pitch,
    std::uint32_t stored_height, std::uint32_t texel, std::uint64_t written) {
    if (!width || !height || !depth || !texel || pitch < width || stored_height < height)
        return std::nullopt;
    const std::uint64_t row_bytes = std::uint64_t{pitch} * texel;
    if (row_bytes > std::numeric_limits<std::uint64_t>::max() / stored_height)
        return std::nullopt;
    const std::uint64_t slice_bytes = row_bytes * stored_height;
    const auto slices = static_cast<std::uint32_t>(std::min<std::uint64_t>(written / slice_bytes, depth));
    const auto tail = slices < depth ? written % slice_bytes : 0;
    const auto rows = static_cast<std::uint32_t>(std::min<std::uint64_t>(tail / row_bytes, height));
    const auto last = rows < height ? std::min(tail % row_bytes, std::uint64_t{width} * texel) : 0;
    if (last % texel) return std::nullopt;

    VolumeFillPlan plan;
    const auto append = [&](std::uint32_t z, std::uint32_t y, std::uint32_t w,
                            std::uint32_t h, std::uint32_t d) {
        if (!w || !h || !d) return;
        const auto offset = std::uint64_t{z} * slice_bytes + std::uint64_t{y} * row_bytes;
        plan.regions[plan.count++] = {offset, z, y, w, h, d};
        plan.buffer_bytes = offset + (d - 1) * slice_bytes + (h - 1) * row_bytes + std::uint64_t{w} * texel;
    };
    append(0, 0, width, height, slices);
    append(slices, 0, width, rows, 1);
    append(slices, rows, static_cast<std::uint32_t>(last / texel), 1, 1);
    return plan;
}

} // namespace Native
