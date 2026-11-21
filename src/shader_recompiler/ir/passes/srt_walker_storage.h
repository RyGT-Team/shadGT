// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <xbyak/xbyak.h>
#include "common/recoverable.h"
#include "shader_recompiler/ir/passes/srt_storage_full.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace Shader {
// The storage must outlive its leases and all signal-handler calls. Production keeps it forever.
// Segments and their addresses never move; only slot ownership is recycled.
// FIX-SGT-094: the segments are made as needed up to a limit set at construction (from the
// machine's memory in production), at most SegmentCount of them. A walker that does not fit
// throws SrtStorageFull after a look at each segment, not a scan of its slots: each segment keeps
// a bound on its longest free run, exact after a failed scan and widened by each release.
template <std::size_t SegmentBytes = 1024 * 1024, std::size_t SegmentCount = 4095>
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
        std::size_t longest_free{}; // FIX-SGT-094: no free run of this segment is longer (slots)
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

    /// `limit_bytes` is rounded down to whole segments, from one segment to SegmentCount.
    explicit SrtWalkerStorage(std::size_t limit_bytes = SegmentBytes * SegmentCount) noexcept
        : limit{(std::clamp)(limit_bytes / SegmentBytes, std::size_t{1}, SegmentCount) * SegmentBytes},
          room{limit} {}

    std::shared_ptr<WalkerLease> Publish(const std::uint8_t* code, std::size_t size) try {
        if (!code || !size || size > limit)
            throw Common::RecoverableFailure{"SRT walker exceeds the executable segment limit"};
        const auto count = (size + SlotBytes - 1) / SlotBytes;
        auto lease = std::shared_ptr<WalkerLease>(new WalkerLease);
        std::unique_lock lock{mutex};
        for (std::size_t n = 0; n < SegmentCount; ++n) {
            auto& segment = segments[n];
            if (!segment.memory) {
                const auto needed = ((size - 1) / SegmentBytes + 1) * SegmentBytes;
                if (needed > cap - reserved) break;
                const auto slots = needed / SlotBytes;
                std::unique_ptr<std::atomic<std::uint64_t>[]> bits;
                std::unique_ptr<std::atomic<std::uint8_t>[]> valid;
                std::unique_ptr<std::atomic<std::uint32_t>[]> ends;
                try {
                    bits = std::make_unique<std::atomic<std::uint64_t>[]>(slots / 64);
                    valid = std::make_unique<std::atomic<std::uint8_t>[]>(slots);
                    ends = std::make_unique<std::atomic<std::uint32_t>[]>(slots);
                    segment.memory = std::make_unique<Xbyak::CodeGenerator>(needed);
                } catch (const Xbyak::Error&) {
                    // FIX-SGT-094: the machine has no memory for another segment: the storage
                    // stays this size (a refusal its callers keep, not one retried per draw).
                    cap = reserved;
                    room.store(0, std::memory_order_relaxed);
                    break;
                } catch (const std::bad_alloc&) {
                    cap = reserved;
                    room.store(0, std::memory_order_relaxed);
                    break;
                }
                segment.bytes = needed; segment.slots = slots; segment.free_slots = slots;
                segment.longest_free = slots;
                segment.live = std::move(bits); segment.valid = std::move(valid);
                segment.ends = std::move(ends);
                reserved += needed;
                made.store(reserved, std::memory_order_relaxed);
                room.store(cap - reserved, std::memory_order_relaxed);
                if (slots > fit_slots.load(std::memory_order_relaxed)) fit_slots.store(slots, std::memory_order_relaxed);
                segment.base.store(segment.memory->getCode(), std::memory_order_release);
                allocated.store(n + 1, std::memory_order_release); // segments are made in order
            }
            if (segment.free_slots < count || segment.longest_free < count) continue;
            std::size_t run = 0, longest = 0;
            // Search from the hint, then wrap; a run never wraps the segment boundary. The steps
            // from slot 0 cover the whole segment, so a failed search measured its longest run.
            for (std::size_t step = 0; step < segment.slots * 2; ++step) {
                ++scanned;
                const auto slot = (segment.hint + step) % segment.slots;
                if (slot == 0) run = 0;
                const bool occupied = (segment.live[slot / 64].load(std::memory_order_relaxed) >> (slot % 64)) & 1;
                run = occupied ? 0 : run + 1;
                longest = (std::max)(longest, run);
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
                const bool widest = (std::min)(segment.free_slots, segment.longest_free) >=
                                    fit_slots.load(std::memory_order_relaxed);
                segment.free_slots -= count;
                segment.longest_free = (std::min)(segment.longest_free, segment.free_slots);
                if (widest) RecomputeFit(); // this segment set MayFit's bound
                segment.hint = (slot + 1) % segment.slots;
                lease->storage = this; lease->segment = n; lease->first = first;
                lease->count = count; lease->code = target;
                live_bytes += count * SlotBytes;
                high_water = (std::max)(high_water, live_bytes);
                ++allocations;
                return lease;
            }
            segment.longest_free = longest;
        }
        RecomputeFit(); // FIX-SGT-094: the segments' bounds were made exact by their scans
        ++failures;
        at_wall.store(true, std::memory_order_relaxed);
        throw SrtStorageFull{size};
    } catch (const std::bad_alloc&) {
        throw Common::RecoverableFailure{"SRT executable storage allocation failed"};
    }
    /// FIX-SGT-094: false when no segment, made or still to make, can take `size` bytes. Lock-free
    /// and without a scan; true is a bound (Publish can still fail, and then makes it exact).
    bool MayFit(std::size_t size) const noexcept {
        if (!size || size > limit) return false;
        const auto count = (size + SlotBytes - 1) / SlotBytes;
        if (count <= fit_slots.load(std::memory_order_relaxed)) return true;
        const auto needed = ((size - 1) / SegmentBytes + 1) * SegmentBytes;
        return allocated.load(std::memory_order_relaxed) < SegmentCount &&
               needed <= room.load(std::memory_order_relaxed);
    }
    /// FIX-SGT-094: a walker was refused and nothing was released since.
    bool AtWall() const noexcept { return at_wall.load(std::memory_order_relaxed); }
    std::size_t Limit() const noexcept { return limit; }
    std::size_t Reserved() const noexcept { return made.load(std::memory_order_relaxed); }
    // Signal-handler safe: no locks, allocations, or dereferences of transient objects.
    std::size_t Available(const void* address) const noexcept {
        const auto pc = reinterpret_cast<std::uintptr_t>(address);
        const auto used = allocated.load(std::memory_order_acquire);
        for (std::size_t n = 0; n < used; ++n) {
            const auto& segment = segments[n];
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
    struct Statistics {
        std::size_t limit; ///< below the constructor's once a segment could not be allocated
        std::size_t reserved, live, high_water, reclaimed, allocations, releases, failures;
        std::uint64_t scanned; ///< slots looked at by Publish's searches
    };
    Statistics Stats() {
        std::scoped_lock lock{mutex};
        return {cap, reserved, live_bytes, high_water, reclaimed_bytes, allocations, releases,
                failures, scanned};
    }
private:
    // MayFit's bound: the longest free run any made segment may have (mutex held).
    void RecomputeFit() noexcept {
        std::size_t fit = 0;
        for (std::size_t n = 0, used = allocated.load(std::memory_order_relaxed); n < used; ++n)
            fit = (std::max)(fit, (std::min)(segments[n].free_slots, segments[n].longest_free));
        fit_slots.store(fit, std::memory_order_relaxed);
    }
    void Release(std::size_t n, std::size_t first, std::size_t count) noexcept {
        std::scoped_lock lock{mutex};
        auto& segment = segments[n];
        for (std::size_t i = first; i < first + count; ++i)
            segment.live[i / 64].fetch_and(~(std::uint64_t{1} << (i % 64)), std::memory_order_release);
        segment.free_slots += count;
        segment.hint = first;
        // FIX-SGT-094: the freed slots join the free ones around them; that run can be the new
        // longest. Whole free words are skipped (slots is a multiple of 64).
        std::size_t begin = first, end = first + count;
        while (begin > 0) {
            const auto last = begin - 1, bit = last % 64;
            const auto below = segment.live[last / 64].load(std::memory_order_relaxed) << (63 - bit);
            if (below) { begin = last + 1 - std::countl_zero(below); break; }
            begin = last - bit;
        }
        while (end < segment.slots) {
            const auto above = segment.live[end / 64].load(std::memory_order_relaxed) >> (end % 64);
            if (above) { end += std::countr_zero(above); break; }
            end += 64 - end % 64;
        }
        segment.longest_free = (std::max)(segment.longest_free, end - begin);
        const auto fit = (std::min)(segment.free_slots, segment.longest_free);
        if (fit > fit_slots.load(std::memory_order_relaxed)) fit_slots.store(fit, std::memory_order_relaxed);
        at_wall.store(false, std::memory_order_relaxed);
        live_bytes -= count * SlotBytes;
        reclaimed_bytes += count * SlotBytes;
        ++releases;
    }
    const std::size_t limit;
    std::mutex mutex;
    std::size_t cap{limit}; // limit, or the size reached when a segment could not be allocated
    std::size_t reserved{};
    std::size_t live_bytes{}, high_water{}, reclaimed_bytes{}, allocations{}, releases{}, failures{};
    std::uint64_t scanned{};
    std::atomic<std::size_t> allocated{}; // segments made, a prefix of the array
    std::atomic<std::size_t> made{};      // reserved, read without the mutex
    std::atomic<std::size_t> room;        // cap - reserved
    std::atomic<std::size_t> fit_slots{}; // no free run of a made segment is longer
    std::atomic<bool> at_wall{};
    std::array<Segment, SegmentCount> segments{};
};
} // namespace Shader
