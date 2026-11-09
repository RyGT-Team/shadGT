#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Native::Parallel {
// One producer publishes completed, immutable slots in stable storage. One
// recording callback consumes them; the storage remains owned through retirement.
class DrawPublication {
    static constexpr std::uint32_t SealedBit=std::uint32_t{1}<<31;
    std::atomic<std::uint32_t> state_{};
public:
    static constexpr std::size_t Capacity=64;
    struct Snapshot {std::size_t count;bool sealed;};
    bool Publish(std::size_t count) noexcept {
        const auto previous=state_.load(std::memory_order_relaxed);
        if((previous&SealedBit) || count<(previous&~SealedBit) || count>Capacity)return false;
        state_.store(static_cast<std::uint32_t>(count),std::memory_order_release);
        state_.notify_one();
        return true;
    }
    Snapshot Load() const noexcept {
        const auto value=state_.load(std::memory_order_acquire);
        return {value&~SealedBit,bool(value&SealedBit)};
    }
    Snapshot Wait(std::size_t consumed) const noexcept {
        auto value=state_.load(std::memory_order_acquire);
        while(!(value&SealedBit) && (value&~SealedBit)<=consumed) {
            state_.wait(value,std::memory_order_acquire);
            value=state_.load(std::memory_order_acquire);
        }
        return {value&~SealedBit,bool(value&SealedBit)};
    }
    // Owner only, after all producer mutation (including storage transfer) ends.
    void Seal() noexcept {
        state_.fetch_or(SealedBit,std::memory_order_release);
        state_.notify_all();
    }
};
}
