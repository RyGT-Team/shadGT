#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <vector>
namespace Native::Parallel {
// Fixed slots remain charged while loaned. A loan is either restored on the
// recorder after failed submission, or fulfilled by the retired Job on worker.
template<class T,std::size_t Slots=16,std::size_t MaxBytes=16*1024*1024,std::size_t MinCapacity=256>
class CompletedDrawVectorPool {
    enum class State:unsigned char {Empty,Seeding,Available,Loaned};
    static_assert(std::atomic<State>::is_always_lock_free && std::atomic<std::size_t>::is_always_lock_free);
    struct Slot {std::atomic<State> state{State::Empty};std::vector<T> vector;std::size_t capacity{};};
    std::array<Slot,Slots> slots_{};std::atomic<std::size_t> bytes_{};
    static bool Bytes(std::size_t capacity,std::size_t& bytes) noexcept {
        if(capacity>MaxBytes/sizeof(T))return false;bytes=capacity*sizeof(T);return true;
    }
public:
    static constexpr std::size_t Invalid=Slots;
    std::size_t TryTake(std::vector<T>& out,std::size_t needed) noexcept {
        if(!out.empty() || out.capacity())return Invalid;
        for(std::size_t i=0;i<Slots;++i) {auto& slot=slots_[i];auto expected=State::Available;
            if(!slot.state.compare_exchange_strong(expected,State::Loaned,std::memory_order_acquire))continue;
            if(slot.capacity!=needed){slot.state.store(State::Available,std::memory_order_release);continue;}
            out.swap(slot.vector);return i;
        }
        return Invalid;
    }
    // Caller has exclusive ownership of this slot's loan; vector is empty and
    // must have the exact capacity originally checked out.
    bool ReturnLoan(std::size_t i,std::vector<T>& v) noexcept {
        if(i>=Slots || !v.empty())return false;auto& slot=slots_[i];
        if(slot.state.load(std::memory_order_relaxed)!=State::Loaned || slot.vector.capacity() || v.capacity()!=slot.capacity)return false;
        slot.vector.swap(v);slot.state.store(State::Available,std::memory_order_release);return true;
    }
    // Only a completed-resource worker calls Seed; clearing owners and any
    // allocation or disposal therefore happen away from the recorder.
    bool Seed(std::vector<T>& v) noexcept {
        v.clear();
        for(auto& slot:slots_) {auto expected=State::Empty;if(!slot.state.compare_exchange_strong(expected,State::Seeding,std::memory_order_acquire))continue;
            std::size_t minimum=0;const auto requested=v.capacity()<MinCapacity?MinCapacity:v.capacity();
            const auto snapshot=bytes_.load(std::memory_order_relaxed);
            if(!Bytes(requested,minimum) || minimum>MaxBytes-snapshot){slot.state.store(State::Empty,std::memory_order_release);return false;}
            try {if(v.capacity()<MinCapacity)v.reserve(MinCapacity);}catch(...) {slot.state.store(State::Empty,std::memory_order_release);return false;}
            std::size_t n=0;if(!Bytes(v.capacity(),n) || !n){slot.state.store(State::Empty,std::memory_order_release);return false;}
            auto old=bytes_.load(std::memory_order_relaxed);
            do {if(n>MaxBytes-old){slot.state.store(State::Empty,std::memory_order_release);return false;}}
            while(!bytes_.compare_exchange_weak(old,old+n,std::memory_order_relaxed));
            slot.capacity=v.capacity();slot.vector.swap(v);slot.state.store(State::Available,std::memory_order_release);return true;
        }
        return false;
    }
};
}
