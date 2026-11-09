// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Vulkan {
// Ordinary stage writes are contiguous. Borrow those ranges instead of copying
// every Vulkan write on every draw; preserve stable grouping for interleaved inputs.
template<class Write, class Scratch, std::size_t Count>
std::array<std::span<const Write>, Count> GroupDescriptorWrites(
    std::span<const Write> writes, std::span<const std::uint8_t> sets,
    std::array<Scratch, Count>& scratch) {
    std::array<std::size_t, Count> first{}, sizes{};
    bool contiguous = true;
    for (std::size_t i = 0; i < writes.size(); ++i) {
        const auto set = i < sets.size() ? sets[i] : 0;
        assert(set < Count);
        if (sizes[set] == 0) first[set] = i;
        else if (first[set] + sizes[set] != i) contiguous = false;
        ++sizes[set];
    }
    std::array<std::span<const Write>, Count> groups{};
    if (contiguous) {
        for (std::size_t set = 0; set < Count; ++set)
            if (sizes[set]) groups[set] = writes.subspan(first[set], sizes[set]);
    } else {
        for (auto& group : scratch) group.clear();
        for (std::size_t i = 0; i < writes.size(); ++i)
            scratch[i < sets.size() ? sets[i] : 0].push_back(writes[i]);
        for (std::size_t set = 0; set < Count; ++set)
            groups[set] = {scratch[set].data(), scratch[set].size()};
    }
    return groups;
}
} // namespace Vulkan
