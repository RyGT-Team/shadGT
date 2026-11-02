#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace Native::Parallel {
// One producer, one completion worker. Worker owns the final shared_ptr releases.
template<std::size_t Slots=64,std::size_t MaxOwners=256>
class SyncOwnerPins {
    enum class State : unsigned char {Free,Reserved,Waiting,Cleaning};
    struct Slot {std::array<std::shared_ptr<const void>,MaxOwners> owners{};std::size_t count{};std::atomic<State> state{State::Free};};
    struct Entry {std::shared_ptr<Slot> slot;std::function<void()> release;std::uint64_t epoch{},tick{};};
    std::array<Entry,Slots> entries_{};
public:
    static constexpr std::size_t Invalid=Slots;
    SyncOwnerPins(){for(auto& entry:entries_)entry.slot=std::make_shared<Slot>();}
    std::size_t Reserve() {
        for(std::size_t i=0;i<Slots;++i){auto& e=entries_[i];if(e.slot->state.load(std::memory_order_acquire)!=State::Free)continue;
            auto owner=e.slot;
            std::function<void()> callback=[owner] {for(std::size_t j=0;j<owner->count;++j)owner->owners[j].reset();owner->count=0;owner->state.store(State::Free,std::memory_order_release);};
            e.release=std::move(callback);e.epoch=0;e.tick=0;e.slot->state.store(State::Reserved,std::memory_order_release);return i;}
        return Invalid;
    }
    bool Pin(std::size_t i,const std::shared_ptr<const void>& owner) noexcept {
        if(i>=Slots || !owner)return false;auto& slot=*entries_[i].slot;
        if(slot.state.load(std::memory_order_relaxed)!=State::Reserved || slot.count>=MaxOwners)return false;
        slot.owners[slot.count++]=owner;return true;
    }
    // Caller proves the queued job holds a copy of every pinned owner.
    void Queued(std::size_t i) noexcept {auto& e=entries_[i];auto& slot=*e.slot;
        for(std::size_t j=0;j<slot.count;++j)slot.owners[j].reset();slot.count=0;e.release={};e.epoch=0;e.tick=0;slot.state.store(State::Free,std::memory_order_release);}
    // Valid only before the first owner is pinned.
    bool CancelEmpty(std::size_t i) noexcept {if(i>=Slots)return false;auto& e=entries_[i];if(e.slot->count)return false;
        e.release={};e.epoch=0;e.tick=0;e.slot->state.store(State::Free,std::memory_order_release);return true;}
    bool Tag(std::size_t i,std::uint64_t tick) noexcept {if(i>=Slots || !tick)return false;auto& e=entries_[i];
        if(e.slot->state.load(std::memory_order_relaxed)!=State::Reserved)return false;e.epoch=tick;e.tick=0;e.slot->state.store(State::Waiting,std::memory_order_release);return true;}
    bool AssignPending(std::uint64_t signal) noexcept {
        if(!signal)return false;
        for(const auto& e:entries_)if(e.slot->state.load(std::memory_order_acquire)==State::Waiting && !e.tick && e.epoch!=signal)return false;
        for(auto& e:entries_)if(e.slot->state.load(std::memory_order_acquire)==State::Waiting && !e.tick)e.tick=signal;
        return true;
    }
    template<class Enqueue> void Collect(std::uint64_t completed,Enqueue&& enqueue) noexcept {
        for(auto& e:entries_)if(e.slot->state.load(std::memory_order_acquire)==State::Waiting && e.tick && e.tick<=completed){
            e.slot->state.store(State::Cleaning,std::memory_order_release);if(enqueue(e.release))e.tick=0;else e.slot->state.store(State::Waiting,std::memory_order_release);}
    }
    bool Waiting() const noexcept {for(const auto& e:entries_)if(e.slot->state.load(std::memory_order_acquire)==State::Waiting)return true;return false;}
};
}