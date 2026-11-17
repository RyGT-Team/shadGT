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
// memory is retired: the prepared draws in it are dropped before the game is told (before the
// event's label is written, which a game may poll, and before the interrupt), and a walk of a
// job that ended before the interrupt can no longer publish into it.

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

/// One stage of a prepared draw, or one program the draw's registers name with its stage enabled:
/// the program's address and its user data registers (a prepared stage's as the job copied them;
/// a draw program's first ones, the ones the program reads).
struct StageIdentity {
    u64 program = 0;
    std::span<const u32> user_data;
};

/// Whether a prepared draw was made for the draw the registers describe: each of its stages is
/// a program an enabled stage names, and the user data registers that program reads hold what
/// the job saw. A prepared draw left under a packet's address and met by another draw written
/// there fails this even with the same shaders: its registers point at its own frame's tables.
/// Registers past the ones a program reads may differ (earlier work leaves anything there).
inline bool PreparedStagesAreTheDraws(std::span<const StageIdentity> prepared,
                                      std::span<const StageIdentity> draw) {
    for (const StageIdentity& stage : prepared) {
        const auto program =
            std::find_if(draw.begin(), draw.end(), [&](const StageIdentity& candidate) {
                return candidate.program == stage.program;
            });
        if (program == draw.end() || stage.user_data.size() < program->user_data.size() ||
            !std::equal(program->user_data.begin(), program->user_data.end(),
                        stage.user_data.begin())) {
            return false;
        }
    }
    return true;
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
/// command memory is certainly its own (right before the game's own job end, which is what lets
/// its frame be submitted); whoever publishes that job's prepared draws later asks RetiredSince:
/// true once the game has had that memory back.
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
