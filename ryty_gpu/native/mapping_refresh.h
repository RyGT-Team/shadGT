// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace Native {
struct MappingRefreshRange { std::uint64_t region, first, end; };
// Only complete pages can be refreshed from the new mapping without overwriting
// GPU-authoritative bytes outside it. Do all bounds checks before publishing.
template<class Regions>
std::optional<std::vector<MappingRefreshRange>> PlanMappingRefresh(
    const Regions& regions, std::uint64_t address, std::uint64_t size) {
    constexpr std::uint64_t page = 4096;
    if (!size || address % page || size % page || address > UINT64_MAX - size)
        return std::nullopt;
    const auto end = address + size;
    std::vector<MappingRefreshRange> plan;
    for (const auto& [base, region] : regions) {
        const auto first = std::max(address, region.begin);
        const auto last = std::min(end, region.end);
        if (first >= last) continue;
        if (first % page || last % page) return std::nullopt;
        plan.push_back({base, first, last});
    }
    return plan;
}
}
