// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
namespace Native::Rhi {
class GpuTimingLease {
public:
    enum State : unsigned char { Free, Recording, Ended, Pending };
    bool TryBegin() {
        auto expected = Free;
        return state.compare_exchange_strong(expected, Recording, std::memory_order_acquire);
    }
    void End() { state.store(Ended, std::memory_order_release); }
    void Publish() { state.store(Pending, std::memory_order_release); }
    bool Readable(std::uint64_t completed, std::uint64_t required) const {
        return state.load(std::memory_order_acquire) == Pending && completed >= required;
    }
    bool IsPending() const { return state.load(std::memory_order_acquire) == Pending; }
    void Release() { state.store(Free, std::memory_order_release); }
    bool Cancel() {
        auto current = state.load(std::memory_order_acquire);
        while (current == Recording || current == Ended) {
            if (state.compare_exchange_weak(current, Free, std::memory_order_release)) return true;
        }
        return false;
    }
private:
    std::atomic<State> state{Free};
};
inline std::uint64_t GpuTimestampDelta(std::uint64_t begin, std::uint64_t end, unsigned bits) {
    if (bits == 0 || bits > 64) return 0;
    return (end - begin) & (bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1);
}
}
