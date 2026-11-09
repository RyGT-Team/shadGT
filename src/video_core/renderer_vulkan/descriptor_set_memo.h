#include "video_core/renderer_vulkan/compile_cpu_batch.h"
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace Vulkan {
// Immutable descriptor sets, scoped to one command list and its descriptor pools.
// Hashes select a slot only: the complete descriptor values must also match.
template<class Set, std::size_t Capacity = 256> class DescriptorSetMemo {
    static constexpr std::size_t Ways = Capacity % 4 == 0 ? 4 : 1;
    static constexpr std::size_t Buckets = Capacity / Ways;
    struct Entry { std::vector<std::uint64_t> key; std::uint64_t hash{}; Set set{}; };
    std::array<Entry, Capacity> entries{};
public:
    Set Find(std::uint64_t hash, std::span<const std::uint64_t> key) const {
        const auto begin = (hash % Buckets) * Ways;
        // Four candidates prevent unrelated per-draw sets from immediately
        // evicting a reusable material set. Full keys still establish equality.
        for (std::size_t i = begin; i < begin + Ways; ++i) {
            const auto& e = entries[i];
            if (e.set && e.hash == hash && std::ranges::equal(e.key, key)) return e.set;
        }
        return Set{};
    }
    void Store(std::uint64_t hash, std::span<const std::uint64_t> key, Set set) {
        const auto bucket = hash % Buckets;
        const auto begin = bucket * Ways;
        std::size_t slot = Capacity;
        for (std::size_t i = begin; i < begin + Ways; ++i) {
            if (entries[i].set && entries[i].hash == hash &&
                std::ranges::equal(entries[i].key, key)) { slot = i; break; }
            if (!entries[i].set) slot = i;
        }
        // Replacement position affects hit rate only, so it can be shared by
        // thread-local users without adding fields to renderer/stream layouts.
        static thread_local std::array<unsigned char, Buckets> next{};
        if (slot == Capacity) slot = begin + next[bucket]++ % Ways;
        auto& e = entries[slot];
        e.key.assign(key.begin(), key.end()); e.hash = hash; e.set = set;
    }
    void Clear() { for (auto& e : entries) { e.set = Set{}; e.key.clear(); } }
};
}
