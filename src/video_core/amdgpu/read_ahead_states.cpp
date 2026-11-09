// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <cstdlib>

#include <xxhash.h>

#include "video_core/amdgpu/read_ahead_states.h"

namespace AmdGpu {

u64 ReadAheadStates::PipelineStateHash(std::span<const u32> regs) {
    std::array<u32, ShGfxCount> sh;
    std::memcpy(sh.data(), regs.data() + ShGfxFirst, sizeof(sh));
    for (u32 stage_base = 0; stage_base < ShGfxCount; stage_base += 0x40) {
        std::fill_n(sh.begin() + stage_base + 0xC, 16, 0u);
    }
    std::array<u32, ContextCount> context;
    std::memcpy(context.data(), regs.data() + ContextFirst, sizeof(context));
    std::array<u32, PrimitiveCount> primitive;
    std::memcpy(primitive.data(), regs.data() + PrimitiveFirst, sizeof(primitive));
    for (const u32 reg : PerDrawRegisters) {
        if (reg >= ContextFirst && reg < ContextFirst + ContextCount) {
            context[reg - ContextFirst] = 0;
        } else if (reg >= PrimitiveFirst && reg < PrimitiveFirst + PrimitiveCount) {
            primitive[reg - PrimitiveFirst] = 0;
        }
    }
    u64 hash = XXH3_64bits(sh.data(), sizeof(sh));
    hash = XXH3_64bits_withSeed(context.data(), sizeof(context), hash);
    return XXH3_64bits_withSeed(primitive.data(), sizeof(primitive), hash);
}

static bool DropReachedDefault() {
    static const bool enabled = [] {
        const char* value = std::getenv("RYGT_READ_AHEAD_DROP_REACHED");
        if (value) return value[0] != '0';
#ifdef RYTY_GPU
        return true;
#else
        return false;
#endif
    }();
    return enabled;
}

ReadAheadStates::ReadAheadStates(size_t capacity_, std::optional<bool> drop_reached_override)
    : capacity{capacity_}, drop_reached{drop_reached_override.value_or(DropReachedDefault())} {
    retired_hashes.reserve(capacity + 1);
    retired_scratch.reserve(capacity + 1);
    free_states.reserve(capacity + 1);
}

bool ReadAheadStates::Offer(std::span<const u32> regs, u64 sequence
#ifndef NDEBUG
                            , ReadAheadPair::Metadata metadata
#endif
                            ) {
    if (regs.size() < RegisterFileDwords) {
        return false;
    }
    bool overflow = false;
    {
        std::scoped_lock lk{mutex};
        retired_hashes.swap(retired_scratch);
        overflow = std::exchange(retired_overflow, false);
    }
    if (overflow) {
        // Only an unexpected feedback overflow clears the set wholesale.
        seen.clear();
        if (seen_epoch == ~u64{0}) dedup_enabled = false;
        else ++seen_epoch;
    }
    for (const auto& retired : retired_scratch)
        if (retired.epoch == seen_epoch) seen.erase(retired.hash);
    retired_scratch.clear();
    if (Space() == 0) return false;
    if (dedup_enabled && seen.size() >= MaxSeen) {
        seen.clear();
        if (seen_epoch == ~u64{0}) dedup_enabled = false;
        else ++seen_epoch;
    }
    const auto hash = PipelineStateHash(regs);
    if (dedup_enabled && !seen.insert(hash).second) {
        return false;
    }
    std::vector<u32> state;
    {
        std::scoped_lock lk{mutex};
        if (!free_states.empty()) {
            state = std::move(free_states.back());
            free_states.pop_back();
        }
    }
    state.resize(StateDwords);
    u32 at = 0;
    for (const auto& range : Ranges) {
        std::memcpy(state.data() + at, regs.data() + range.first, range.count * sizeof(u32));
        at += range.count;
    }
    {
        std::scoped_lock lk{mutex};
        queue.push_back({std::move(state), sequence, hash, seen_epoch
#ifndef NDEBUG
                         , metadata
#endif
                         });
        size.fetch_add(1, std::memory_order_release);
    }
    offered.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool ReadAheadStates::Take(std::span<u32> regs
#ifndef NDEBUG
                           , ReadAheadPair::Metadata* metadata
#endif
                           ) {
    if (regs.size() < RegisterFileDwords) {
        return false;
    }
    std::vector<u32> state;
    bool found = false;
    for (size_t inspected = 0; inspected < capacity; ++inspected) {
        std::scoped_lock lk{mutex};
        if (queue.empty()) return false;
        auto& front = queue.front();
        if (drop_reached && front.sequence &&
            front.sequence <= recorder_started.load(std::memory_order_acquire)) {
            if (retired_hashes.size() < retired_hashes.capacity())
                retired_hashes.push_back({front.hash, front.epoch});
            else
                retired_overflow = true;
            free_states.push_back(std::move(front.registers));
            queue.pop_front();
            size.fetch_sub(1, std::memory_order_release);
#ifndef NDEBUG
            ++drop_stats.dropped;
#endif
            continue;
        }
#ifndef NDEBUG
        if (metadata) *metadata = front.metadata;
        ++(front.sequence ? drop_stats.returned_decoded : drop_stats.returned_scan);
#endif
        state = std::move(front.registers);
        queue.pop_front();
        size.fetch_sub(1, std::memory_order_release);
        found = true;
        break;
    }
    if (!found) return false;
    u32 at = 0;
    for (const auto& range : Ranges) {
        std::memcpy(regs.data() + range.first, state.data() + at, range.count * sizeof(u32));
        at += range.count;
    }
    std::scoped_lock lk{mutex};
    free_states.push_back(std::move(state));
    return true;
}

} // namespace AmdGpu
