// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <span>
#include <unordered_map>
#include <vector>
namespace Vulkan {
// Never-written sets only; issued sets cannot be returned. Clear with the
// command list's pools before they become eligible for GPU-safe recycling.
template<class Layout, class Set, std::size_t Limit = 64> class DescriptorSetReserve {
    struct Entry { unsigned misses = 0; std::vector<Set> unused; };
    std::unordered_map<Layout, Entry> entries;
    std::size_t spare = 0;
public:
    static constexpr std::size_t SpareLimit = Limit;
    Set Take(Layout layout) {
        const auto it = entries.find(layout);
        if (it == entries.end() || it->second.unused.empty()) return {};
        auto& unused = it->second.unused;
        const Set result = unused.back(); unused.pop_back(); --spare;
        return result;
    }
    std::size_t BatchSize(Layout layout, std::size_t pending_extra = 0) {
        auto& entry = entries[layout];
        if (++entry.misses == 1 || spare + pending_extra >= Limit) return 1;
        return std::min<std::size_t>(32, 1 + Limit - spare - pending_extra);
    }
    void Store(Layout layout, std::span<const Set> sets) {
        auto& unused = entries[layout].unused;
        const auto count = std::min(sets.size(), Limit - spare);
        unused.insert(unused.end(), sets.begin(), sets.begin() + count);
        spare += count;
    }
    void Clear() { entries.clear(); spare = 0; }
    std::size_t SpareCount() const { return spare; }
};
}
