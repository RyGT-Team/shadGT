// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include "upload_bytes_equal.h"

namespace Vulkan {
// Immutable uploads belong to one command list. Hashes select candidates only;
// full CPU-byte equality and stage identity establish reuse. No upload-heap reads.
template<class Binding, std::size_t Stages, std::size_t Slots = 128,
         std::size_t MaxBytes = 64 * 1024>
class ImmutableUploadMemo {
    static constexpr std::size_t Ways = 4, Buckets = Slots / Ways;
    static_assert(Slots >= Ways && Slots % Ways == 0);
    struct Entry {
        bool valid{},backing_proven{};
        std::uint32_t stage{};
        std::uint64_t hash{};
        std::vector<std::uint8_t> bytes;
        Binding binding{};
    };
    std::array<Entry, Slots> entries{};
    std::array<std::size_t, Stages> last{};
    std::array<unsigned char, Buckets> replacement{};
public:
    struct Lookup {
        std::optional<Binding> binding;
        std::size_t slot = Slots;
        std::uint64_t hash{};
        bool backing_proven{};
    };
    ImmutableUploadMemo() { last.fill(Slots); }

    template<class Hash> Lookup Find(std::uint32_t stage, std::span<const std::uint8_t> bytes,
                                    Hash&& hash_bytes) {
        if (stage >= Stages || bytes.empty() || bytes.size() > MaxBytes) return {};
        const auto equal = [&](const Entry& entry) {
            return entry.valid && entry.stage == stage && entry.bytes.size() == bytes.size() &&
                   UploadBytesEqual(entry.bytes.data(), bytes.data(), bytes.size());
        };
        if (last[stage] < Slots && equal(entries[last[stage]])) {
            const auto& entry = entries[last[stage]];
            return {entry.binding, last[stage], entry.hash, entry.backing_proven};
        }
        const std::uint64_t hash = hash_bytes(bytes.data(), bytes.size());
        const auto bucket = (hash ^ (std::uint64_t(stage) * 0x9e3779b97f4a7c15ull)) % Buckets;
        const auto begin = bucket * Ways;
        auto vacant = Slots;
        for (std::size_t i = begin; i < begin + Ways; ++i) {
            if (!entries[i].valid) vacant = i;
            if (entries[i].hash == hash && equal(entries[i])) {
                last[stage] = i;
                return {entries[i].binding, i, hash, entries[i].backing_proven};
            }
        }
        const auto slot = vacant != Slots ? vacant : begin + replacement[bucket]++ % Ways;
        return {std::nullopt, slot, hash};
    }

    // Predicate is called synchronously while the current list owns this entry.
    template<class Predicate>
    std::optional<Binding> WithLast(std::uint32_t stage, std::size_t size, Predicate&& predicate) const {
        if (stage >= Stages || last[stage] >= Slots) return std::nullopt;
        const auto& entry = entries[last[stage]];
        if (!entry.valid || entry.stage != stage || entry.bytes.size() != size) return std::nullopt;
        if (!predicate(std::span<const std::uint8_t>{entry.bytes})) return std::nullopt;
        return entry.binding;
    }

    bool Store(const Lookup& lookup, std::uint32_t stage,
               std::span<const std::uint8_t> bytes, Binding binding, bool backing_proven = false) {
        if (lookup.slot >= Slots || stage >= Stages || bytes.empty() || bytes.size() > MaxBytes)
            return false;
        auto& entry = entries[lookup.slot];
        entry.valid = false;
        // Avoid geometric vector growth beyond the explicit per-entry bound.
        if (bytes.size() > entry.bytes.capacity()) {
            std::vector<std::uint8_t>(bytes.begin(), bytes.end()).swap(entry.bytes);
            if (entry.bytes.capacity() > MaxBytes) {
                std::vector<std::uint8_t>().swap(entry.bytes);
                return false;
            }
        } else {
            entry.bytes.assign(bytes.begin(), bytes.end());
        }
        entry.stage = stage; entry.hash = lookup.hash; entry.binding = binding; entry.backing_proven = backing_proven;
        entry.valid = true; last[stage] = lookup.slot;
        return true;
    }

    void Clear() {
        last.fill(Slots); replacement.fill(0);
        for (auto& entry : entries) { entry.valid = false; entry.binding = {}; entry.bytes.clear(); }
    }
    std::size_t RetainedBytes() const {
        std::size_t total = 0;
        for (const auto& entry : entries) total += entry.bytes.capacity();
        return total;
    }
};
} // namespace Vulkan
