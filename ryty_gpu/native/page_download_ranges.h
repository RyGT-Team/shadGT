// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>
namespace Native {
using PageDownloadRange = std::pair<std::uint64_t, std::uint64_t>;
inline void AddPageDownloadRange(std::vector<PageDownloadRange>& ranges,
                                 std::uint64_t begin, std::uint64_t end) {
    if (begin >= end) return;
    auto it = ranges.begin();
    while (it != ranges.end() && it->second < begin) ++it;
    while (it != ranges.end() && it->first <= end) {
        begin = std::min(begin, it->first);
        end = std::max(end, it->second);
        it = ranges.erase(it);
    }
    ranges.insert(it, {begin, end});
}
}
