#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>
namespace Native {
struct RegionInitRange { std::uint64_t begin{}, end{}; };
enum class RegionMergeDisposition { Replace, Preserve };
inline RegionMergeDisposition RegionMergePolicy(bool valid, bool, std::uint64_t, std::uint64_t) {
    if (valid) return RegionMergeDisposition::Replace;
    // Refuse an overlapping write rather than discard unaffected GPU-only bytes.
    return RegionMergeDisposition::Preserve;
}
// The allocation callback runs before any region is removed, so failure leaves the map intact.
template <class Map, class Validate, class Allocate, class Removed>
RegionMergeDisposition ReplaceOverlappingRegions(Map& regions, std::uint64_t& begin,
    std::uint64_t& end, bool write, std::uint64_t write_size, std::uint64_t max_size,
    Validate validate, Allocate allocate, Removed removed) {
    for (const auto& [base, region] : regions) {
        if (region.begin < end && region.end > begin) {
            begin = std::min(begin, region.begin);
            end = std::max(end, region.end);
        }
    }
    const auto action = RegionMergePolicy(validate(begin, end), write, write_size, max_size);
    if (action == RegionMergeDisposition::Preserve) return action;
    if (action == RegionMergeDisposition::Replace && !allocate(begin, end))
        return RegionMergeDisposition::Preserve;
    for (auto it = regions.begin(); it != regions.end();) {
        if (it->second.begin < end && it->second.end > begin) {
            removed(std::move(it->second));
            it = regions.erase(it);
        } else ++it;
    }
    return action;
}
inline std::vector<RegionInitRange> RegionInitGaps(
    std::uint64_t begin, std::uint64_t end, std::span<const RegionInitRange> covered) {
    std::vector<RegionInitRange> ordered, gaps;
    if (end <= begin) return gaps;
    ordered.reserve(covered.size());
    for (const auto range : covered) {
        const auto lo = std::max(begin,range.begin), hi = std::min(end,range.end);
        if (lo < hi) ordered.push_back({lo,hi});
    }
    std::sort(ordered.begin(),ordered.end(),[](auto a,auto b) { return a.begin < b.begin; });
    auto cursor = begin;
    for (const auto range : ordered) {
        if (cursor < range.begin) gaps.push_back({cursor,range.begin});
        cursor = std::max(cursor,range.end);
    }
    if (cursor < end) gaps.push_back({cursor,end});
    return gaps;
}
}
