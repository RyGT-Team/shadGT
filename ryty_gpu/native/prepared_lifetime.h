// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// FIX-GTS-492: how long a prepared draw (PERF-316) may live.
//
// GT Sport writes a frame's draw packets, and the tables its draws read (V#s, constants), into
// command memory it hands back to its pools at that frame's end-of-pipe interrupt: jobs of later
// frames then write other commands and other tables at the same addresses. A prepared draw is
// kept under its draw packet's address, so it is only that packet's draw until the interrupt.
// One left in the table past it (never taken, or published by a walk that ran late) was installed
// for whatever draw came next at that address: another draw's pipeline and registers, V#s read
// through a pointer of a frame long gone. A cold GT Sport race lost the GPU device that way
// (2026-11-16: a prepared draw 1,384 submissions old, a tessellated pipeline on a plain draw).
//
// The command thread notes the command memory it decodes; at each end-of-pipe interrupt that
// memory is retired: the prepared draws in it are dropped before the game is told, and a walk
// that began before the interrupt can no longer publish into it.

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include "common/types.h"

namespace Native {

/// A prepared draw this many graphics submissions old was made by a job whose commands never
/// came to the command thread (the retirement at the interrupt cannot name its memory): it goes
/// at the next interrupt. GT Sport submits twice a frame and prepares up to two frames ahead
/// (ages 2 and 4 in a race); 64 submissions is half a second of race.
constexpr u32 MaxPreparedDrawAge = 64;

/// Whether a prepared draw made at submission count `made` is past that age at `now`.
constexpr bool PreparedDrawTooOld(u32 made, u32 now) noexcept {
    return static_cast<u32>(now - made) > MaxPreparedDrawAge;
}

/// A stretch of command memory, [begin, end).
struct CommandRange {
    u64 begin = 0;
    u64 end = 0;
};

/// Sorts the ranges and merges the ones that overlap or touch; empty ranges go.
inline void NormalizeCommandRanges(std::vector<CommandRange>& ranges) {
    std::erase_if(ranges, [](const CommandRange& range) { return range.end <= range.begin; });
    std::sort(ranges.begin(), ranges.end(),
              [](const CommandRange& a, const CommandRange& b) { return a.begin < b.begin; });
    size_t kept = 0;
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (kept != 0 && ranges[i].begin <= ranges[kept - 1].end) {
            ranges[kept - 1].end = std::max(ranges[kept - 1].end, ranges[i].end);
        } else {
            ranges[kept++] = ranges[i];
        }
    }
    ranges.resize(kept);
}

/// Whether `address` lies in one of the normalized ranges.
inline bool InCommandRanges(std::span<const CommandRange> normalized, u64 address) {
    const auto after = std::upper_bound(
        normalized.begin(), normalized.end(), address,
        [](u64 value, const CommandRange& range) { return value < range.begin; });
    return after != normalized.begin() && address < std::prev(after)->end;
}

/// The command memory retired at the last end-of-pipe interrupts. A job reads Serial() while its
/// command memory is certainly its own (as it ends); whoever publishes that job's prepared draws
/// later asks RetiredSince: true once the game has had that memory back.
class RetiredCommandMemory {
public:
    static constexpr u64 Kept = 8;

    u64 Serial() const noexcept {
        return serial.load(std::memory_order_acquire);
    }

    /// Records a retirement; `ranges` comes back normalized. The caller drops what it holds in
    /// them after this call: a publisher that read the old serial is then either dropped by the
    /// caller or refused by RetiredSince.
    void Retire(std::vector<CommandRange>& ranges) {
        NormalizeCommandRanges(ranges);
        std::unique_lock lock{mutex};
        const u64 next = serial.load(std::memory_order_relaxed) + 1;
        Set& set = sets[next % Kept];
        set.serial = next;
        set.ranges = ranges;
        serial.store(next, std::memory_order_release);
    }

    /// Whether `address` was retired after the retirement numbered `since`. Also true when the
    /// retirements since then are no longer all kept: too long ago to tell.
    bool RetiredSince(u64 since, u64 address) const {
        if (serial.load(std::memory_order_acquire) == since) {
            return false;
        }
        std::shared_lock lock{mutex};
        const u64 now = serial.load(std::memory_order_relaxed);
        if (since > now || now - since > Kept) {
            return true;
        }
        for (u64 number = since + 1; number <= now; ++number) {
            const Set& set = sets[number % Kept];
            if (set.serial != number || InCommandRanges(set.ranges, address)) {
                return true;
            }
        }
        return false;
    }

private:
    struct Set {
        u64 serial = 0;
        std::vector<CommandRange> ranges;
    };
    std::atomic<u64> serial{0};
    mutable std::shared_mutex mutex;
    std::array<Set, Kept> sets{};
};

} // namespace Native
