// Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace Native {

// Retain the stream's original owners until the scheduler has accepted a copy.
template <class T, class Enqueue>
void EnqueueRetainedReceipts(std::vector<std::shared_ptr<T>>& receipts, Enqueue&& enqueue) {
    auto copy = receipts;
    enqueue(std::move(copy));
    receipts.clear();
}

// The caller serializes journal changes with target publication and CPU invalidation.
class RecentFills {
public:
    using Address = std::uint64_t;
    using Pattern = std::array<std::uint32_t, 2>;
    struct Fill {
        Address begin{};
        Address end{};
        Pattern pattern{};
        std::uint64_t frame{};
        std::uint64_t clock{};
    };

    bool Forget(Address begin, std::uint64_t size) {
        const Address end = End(begin, size);
        bool changed = false;
        for (auto& fill : fills) {
            if (fill.end && fill.begin < end && begin < fill.end) {
                fill.end = 0;
                changed = true;
            }
        }
        return changed;
    }

    void Record(Address begin, std::uint64_t size, Pattern pattern,
                std::uint64_t frame, std::uint64_t clock) {
        Forget(begin, size);
        if (!size || size > std::numeric_limits<Address>::max() - begin) return;
        fills[next++ % fills.size()] = {begin, End(begin, size), pattern, frame, clock};
    }

    std::optional<Fill> Find(Address begin, std::uint64_t size, std::uint64_t frame) const {
        if (!size || size > std::numeric_limits<Address>::max() - begin) return {};
        std::optional<Fill> found;
        for (const auto& fill : fills) {
            if (fill.end && fill.frame == frame && fill.begin <= begin &&
                fill.end >= End(begin, size) && (!found || fill.clock > found->clock)) {
                // The fill pattern is 64 bits. Sub-dword offsets cannot be represented by
                // the two-word image fill interface; fall back to guest memory for those.
                const auto offset = begin - fill.begin;
                if (offset % 4) continue;
                found = fill;
                if (offset % 8) std::swap(found->pattern[0], found->pattern[1]);
            }
        }
        return found;
    }

    std::vector<std::pair<Address, Address>> Ranges() const {
        std::vector<std::pair<Address, Address>> ranges;
        for (const auto& fill : fills) if (fill.end) ranges.emplace_back(fill.begin, fill.end);
        return ranges;
    }

    void Expire(std::uint64_t frame) {
        for (auto& fill : fills) if (fill.end && fill.frame != frame) fill.end = 0;
    }

private:
    static Address End(Address begin, std::uint64_t size) {
        return size > std::numeric_limits<Address>::max() - begin
                   ? std::numeric_limits<Address>::max() : begin + size;
    }
    std::array<Fill, 16> fills{};
    std::size_t next{};
};

} // namespace Native
