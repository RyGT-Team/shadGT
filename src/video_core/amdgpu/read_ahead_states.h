// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/types.h"
#ifndef NDEBUG
#include "video_core/amdgpu/read_ahead_pair_trace.h"
#endif

namespace AmdGpu {

/// PERF-034: the pipeline read-ahead (PERF-019) with the draw pipe (PERF-031). Pipelines are
/// looked up and built from the recorder thread, which cannot read the command buffers the
/// command thread is decoding. So the command thread finds the upcoming draws whose pipelines
/// may be new (the draws it decodes, and while new pipelines keep appearing the commands ahead
/// of it) and queues their register states here. The pipeline cache takes them on the recorder
/// thread and starts their builds on worker threads before the recorder reaches the draws.
///
/// A queued state holds the register ranges pipeline keys are built from (PERF-023: config,
/// graphics shader, context and uconfig registers; 34 KB).
class ReadAheadStates {
public:
    struct Range {
        u32 first;
        u32 count;
    };
    static constexpr std::array<Range, 4> Ranges{{
        {0x2000, 0xC00},  // config
        {0x2C00, 0x200},  // graphics shader
        {0xA000, 0x400},  // context
        {0xC000, 0x1000}, // uconfig
    }};
    static constexpr u32 StateDwords = [] {
        u32 dwords = 0;
        for (const auto& range : Ranges) {
            dwords += range.count;
        }
        return dwords;
    }();
    /// The register file the states are read from and written to has at least this many dwords.
    static constexpr u32 RegisterFileDwords = 0xD000;

    static constexpr u32 ShGfxFirst = 0x2C00;
    static constexpr u32 ShGfxCount = 0x200;
    static constexpr u32 ContextFirst = 0xA000;
    static constexpr u32 ContextCount = 0x400;
    static constexpr u32 PrimitiveFirst = 0xC200;
    static constexpr u32 PrimitiveCount = 0x100;

    /// Whether a register is graphics shader user data: each stage's 16 user data registers
    /// start 0xC after its program registers, in blocks of 0x40 (PERF-022).
    static constexpr bool IsUserData(u32 reg) {
        if (reg < ShGfxFirst || reg >= ShGfxFirst + ShGfxCount) {
            return false;
        }
        const u32 in_stage = (reg - ShGfxFirst) & 0x3F;
        return in_stage >= 0xC && in_stage < 0xC + 16;
    }

    /// PERF-037: registers the draw packets themselves write on the command thread (index base
    /// and size, the draw initiator, the index count). They differ on nearly every draw and no
    /// pipeline key reads them; the scan-ahead (ScanPackets) never sees them either. Hashing
    /// them made every decoded draw a new state (3.5 million evaluated in one session for
    /// 2,204 builds). Liverpool checks these against the Regs fields.
    static constexpr std::array<u32, 5> PerDrawRegisters{
        0xA1F9, 0xA1FA, // index_base_address
        0xA1FC,         // draw_initiator
        0xA29E,         // max_index_size
        0xC24C,         // num_indices
    };
    static constexpr bool IsPerDraw(u32 reg) {
        for (const u32 per_draw : PerDrawRegisters) {
            if (reg == per_draw) {
                return true;
            }
        }
        return false;
    }

    /// Whether writing [first, first + count) can change what PipelineStateHash hashes.
    static constexpr bool AffectsPipeline(u32 first, u32 count) {
        const u32 end = first + count;
        const auto any_in = [&](u32 range_first, u32 range_count, auto&& ignored) {
            const u32 lo = std::max(first, range_first);
            const u32 hi = std::min(end, range_first + range_count);
            for (u32 reg = lo; reg < hi; ++reg) {
                if (!ignored(reg)) {
                    return true;
                }
            }
            return false;
        };
        return any_in(ContextFirst, ContextCount, IsPerDraw) ||
               any_in(PrimitiveFirst, PrimitiveCount, IsPerDraw) ||
               any_in(ShGfxFirst, ShGfxCount, IsUserData);
    }

    /// What decides a draw's pipeline that commands can change (PERF-021): the graphics shader
    /// registers without user data (PERF-022: per-draw constants and resource pointers), the
    /// context registers and the primitive registers, without the per-draw packet registers.
    static u64 PipelineStateHash(std::span<const u32> regs);

    explicit ReadAheadStates(size_t capacity = 512, std::optional<bool> drop_reached_override = std::nullopt);

    /// Command thread: queues the state unless a state with the same hash was queued before
    /// (or the queue is full). Returns whether it was queued.
    bool Offer(std::span<const u32> regs, u64 sequence = 0
#ifndef NDEBUG
               , ReadAheadPair::Metadata metadata = {}
#endif
               );

    /// Producer only. Zero means unsequenced (including forward-scanned states).
    u64 NextDrawSequence() noexcept {
        if (next_draw_sequence == ~u64{0}) return 0;
        return ++next_draw_sequence;
    }
    /// Recorder callback, before applying the draw's register delta.
    void NoteRecorderStarted(u64 sequence) noexcept {
        auto started = recorder_started.load(std::memory_order_relaxed);
        while (started < sequence && !recorder_started.compare_exchange_weak(
                   started, sequence, std::memory_order_release, std::memory_order_relaxed)) {}
    }
#ifndef NDEBUG
    struct DropStats { u64 dropped{}, returned_decoded{}, returned_scan{}; };
    DropStats TakeDropStats() noexcept { return std::exchange(drop_stats, {}); }
#endif

    /// How many more states fit. Exact on the command thread, the only one that adds.
    size_t Space() const {
        const size_t queued = size.load(std::memory_order_acquire);
        return queued < capacity ? capacity - queued : 0;
    }

    bool Empty() const {
        return size.load(std::memory_order_acquire) == 0;
    }

    /// Recorder thread: writes the oldest queued state into `regs` (registers outside the
    /// ranges keep their values). False when nothing is queued.
    bool Take(std::span<u32> regs
#ifndef NDEBUG
              , ReadAheadPair::Metadata* metadata = nullptr
#endif
              );

    /// Recorder thread: a draw needed a pipeline that was not built yet.
    void NoteMiss() {
        last_miss_ns.store(NowNs(), std::memory_order_relaxed);
        misses.fetch_add(1, std::memory_order_release);
    }

    /// Command thread: whether a miss was noted since the previous call.
    bool TakeNewMiss() {
        const u64 now_misses = misses.load(std::memory_order_acquire);
        return std::exchange(misses_seen, now_misses) != now_misses;
    }

    /// Whether a miss was noted within `window`.
    bool MissWithin(std::chrono::nanoseconds window) const {
        const s64 last = last_miss_ns.load(std::memory_order_relaxed);
        return last != 0 && NowNs() - last <= window.count();
    }

    u64 TakeOffered() {
        return offered.exchange(0, std::memory_order_relaxed);
    }

private:
    static s64 NowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static constexpr size_t MaxSeen = size_t{1} << 16;
    const size_t capacity;
    const bool drop_reached;
    u64 next_draw_sequence{}; // producer only
    std::atomic<u64> recorder_started{};
    // Command thread only.
    std::unordered_set<u64> seen;
    u64 seen_epoch{1};
    bool dedup_enabled{true};
    u64 misses_seen{};

    std::mutex mutex;
    struct RetiredHash { u64 hash{}, epoch{}; };
    std::vector<RetiredHash> retired_hashes; // bounded, consumer writes; producer drains
    std::vector<RetiredHash> retired_scratch; // producer only; exchanged under mutex
    bool retired_overflow{}; // rare bounded fallback, guarded by mutex
    struct QueuedState { std::vector<u32> registers; u64 sequence{}, hash{}, epoch{};
#ifndef NDEBUG
        ReadAheadPair::Metadata metadata;
#endif
    };
    std::deque<QueuedState> queue;
#ifndef NDEBUG
    DropStats drop_stats{}; // consumer only
#endif
    std::vector<std::vector<u32>> free_states;
    std::atomic<size_t> size{};
    std::atomic<u64> offered{};
    std::atomic<u64> misses{};
    std::atomic<s64> last_miss_ns{};
};

} // namespace AmdGpu
