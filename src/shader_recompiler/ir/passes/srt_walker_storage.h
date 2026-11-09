// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <xbyak/xbyak.h>
#include "common/recoverable.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace Shader {
// The storage must outlive its leases and all signal-handler calls. Production keeps it forever.
// Segments and their addresses never move; only slot ownership is recycled.
template <std::size_t SegmentBytes = 1024 * 1024, std::size_t SegmentCount = 256>
class SrtWalkerStorage {
    static constexpr std::size_t SlotBytes = 64;
    static_assert(SegmentBytes && SegmentBytes % (SlotBytes * 64) == 0);
    static_assert(SegmentCount <= UINT32_MAX / SegmentBytes);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<const std::uint8_t*>::is_always_lock_free);
    struct Segment {
        std::unique_ptr<Xbyak::CodeGenerator> memory;
        std::atomic<const std::uint8_t*> base{};
        std::unique_ptr<std::atomic<std::uint64_t>[]> live;
        std::unique_ptr<std::atomic<std::uint8_t>[]> valid;
        std::unique_ptr<std::atomic<std::uint32_t>[]> ends;
        std::size_t bytes{}, slots{}, free_slots{}, hint{};
    };
public:
    class WalkerLease {
    public:
        ~WalkerLease() noexcept { if (storage) storage->Release(segment, first, count); }
        const std::uint8_t* Code() const noexcept { return code; }
        WalkerLease(const WalkerLease&) = delete;
        WalkerLease& operator=(const WalkerLease&) = delete;
    private:
        friend class SrtWalkerStorage;
        WalkerLease() = default;
        SrtWalkerStorage* storage{};
        std::size_t segment{}, first{}, count{};
        const std::uint8_t* code{};
    };

    std::shared_ptr<WalkerLease> Publish(const std::uint8_t* code, std::size_t size) try {
        if (!code || !size || size > SegmentBytes * SegmentCount)
            throw Common::RecoverableFailure{"SRT walker exceeds the executable segment limit"};
        const auto count = (size + SlotBytes - 1) / SlotBytes;
        auto lease = std::shared_ptr<WalkerLease>(new WalkerLease);
        std::unique_lock lock{mutex};
        for (std::size_t n = 0; n < SegmentCount; ++n) {
            auto& segment = segments[n];
            if (!segment.memory) {
                const auto needed = ((size - 1) / SegmentBytes + 1) * SegmentBytes;
                if (needed > SegmentBytes * SegmentCount - reserved) break;
                const auto slots = needed / SlotBytes;
                auto bits = std::make_unique<std::atomic<std::uint64_t>[]>(slots / 64);
                auto valid = std::make_unique<std::atomic<std::uint8_t>[]>(slots);
                auto ends = std::make_unique<std::atomic<std::uint32_t>[]>(slots);
                try { segment.memory = std::make_unique<Xbyak::CodeGenerator>(needed); }
                catch (const Xbyak::Error&) { throw Common::RecoverableFailure{"SRT executable segment allocation failed"}; }
                segment.bytes = needed; segment.slots = slots; segment.free_slots = slots;
                segment.live = std::move(bits); segment.valid = std::move(valid);
                segment.ends = std::move(ends);
                reserved += needed;
                segment.base.store(segment.memory->getCode(), std::memory_order_release);
            }
            if (segment.free_slots < count) continue;
            std::size_t run = 0;
            // Search from the hint, then wrap; a run never wraps the segment boundary.
            for (std::size_t step = 0; step < segment.slots * 2; ++step) {
                const auto slot = (segment.hint + step) % segment.slots;
                if (slot == 0) run = 0;
                const bool occupied = (segment.live[slot / 64].load(std::memory_order_relaxed) >> (slot % 64)) & 1;
                run = occupied ? 0 : run + 1;
                if (run != count) continue;
                const auto first = slot + 1 - count;
                auto* target = const_cast<std::uint8_t*>(segment.base.load(std::memory_order_relaxed)) + first * SlotBytes;
                std::memcpy(target, code, size);
 #ifdef _WIN32
                if (!FlushInstructionCache(GetCurrentProcess(), target, size))
                    throw Common::RecoverableFailure{"SRT instruction cache flush failed"};
 #else
                __builtin___clear_cache(reinterpret_cast<char*>(target), reinterpret_cast<char*>(target + size));
 #endif
                for (auto i = first; i <= slot; ++i) {
                    const auto offset = (i - first) * SlotBytes;
                    segment.valid[i].store(static_cast<std::uint8_t>((std::min)(SlotBytes, size - offset)), std::memory_order_relaxed);
                    segment.ends[i].store(static_cast<std::uint32_t>(first * SlotBytes + size), std::memory_order_relaxed);
                    segment.live[i / 64].fetch_or(std::uint64_t{1} << (i % 64), std::memory_order_release);
                }
                segment.free_slots -= count;
                segment.hint = (slot + 1) % segment.slots;
                lease->storage = this; lease->segment = n; lease->first = first;
                lease->count = count; lease->code = target;
#ifndef NDEBUG
                const auto live_now = (live_bytes += count * SlotBytes);
                if (live_now > high_water) high_water = live_now;
                ++allocations;
#endif
                return lease;
            }
        }
        throw Common::RecoverableFailure{"SRT executable storage exhausted"};
    } catch (const std::bad_alloc&) {
        throw Common::RecoverableFailure{"SRT executable storage allocation failed"};
    }
    // Signal-handler safe: no locks, allocations, or dereferences of transient objects.
    std::size_t Available(const void* address) const noexcept {
        const auto pc = reinterpret_cast<std::uintptr_t>(address);
        for (const auto& segment : segments) {
            const auto base = reinterpret_cast<std::uintptr_t>(segment.base.load(std::memory_order_acquire));
            if (!base || pc < base || pc - base >= segment.bytes) continue;
            const auto slot = (pc - base) / SlotBytes;
            if (!((segment.live[slot / 64].load(std::memory_order_acquire) >> (slot % 64)) & 1)) return 0;
            if ((pc - base) % SlotBytes >= segment.valid[slot].load(std::memory_order_relaxed)) return 0;
            return segment.ends[slot].load(std::memory_order_relaxed) - (pc - base);
        }
        return 0;
    }
    bool Contains(const void* address) const noexcept { return Available(address) != 0; }
#ifndef NDEBUG
    struct Statistics { std::size_t reserved, live, high_water, reclaimed, allocations, releases; };
    Statistics Stats() {
        std::scoped_lock lock{mutex};
        return {reserved, live_bytes, high_water, reclaimed_bytes, allocations, releases};
    }
#endif
private:
    void Release(std::size_t n, std::size_t first, std::size_t count) noexcept {
        std::scoped_lock lock{mutex};
        auto& segment = segments[n];
        for (std::size_t i = first; i < first + count; ++i)
            segment.live[i / 64].fetch_and(~(std::uint64_t{1} << (i % 64)), std::memory_order_release);
        segment.free_slots += count;
        segment.hint = first;
#ifndef NDEBUG
        live_bytes -= count * SlotBytes;
        reclaimed_bytes += count * SlotBytes;
        ++releases;
#endif
    }
    std::mutex mutex;
    std::size_t reserved{};
#ifndef NDEBUG
    std::size_t live_bytes{}, high_water{}, reclaimed_bytes{}, allocations{}, releases{};
#endif
    std::array<Segment, SegmentCount> segments{};
};
} // namespace Shader
