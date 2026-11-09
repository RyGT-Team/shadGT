// SPDX-License-Identifier: GPL-2.0-or-later
#include "ryty_gpu/native/sync_owner_pins.h"
#include <cassert>
#include <vector>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace Reference {
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
template<class Pins> std::vector<int> Exercise() {
    Pins pins; std::vector<int> log; std::vector<std::function<void()>> callbacks;
    bool accept=false, synchronous=false;
    auto enqueue=[&](auto& callback) noexcept {
        log.push_back(10); if(!accept)return false;
        if(synchronous)callback(); else callbacks.push_back(std::move(callback)); return true;
    };
    auto collect=[&](unsigned tick){pins.Collect(tick,enqueue);log.push_back(pins.Waiting());};
    collect(0);
    auto a=pins.Reserve(),b=pins.Reserve();log.push_back(int(a));log.push_back(int(b));
    auto owner=std::make_shared<int>(4);
    assert(pins.Pin(a,owner));assert(pins.Tag(a,5));assert(pins.Tag(b,5));
    collect(100);assert(pins.Waiting());
    log.push_back(pins.AssignPending(4));log.push_back(pins.AssignPending(5));
    collect(4);collect(5);accept=true;collect(5);
    assert(!pins.Waiting());assert(callbacks.size()==2);assert(pins.Reserve()==Pins::Invalid);
    callbacks[0]();callbacks[1]();callbacks.clear();
    auto c=pins.Reserve(),d=pins.Reserve();log.push_back(int(c));log.push_back(int(d));
    assert(pins.Tag(c,7));assert(pins.Tag(d,7));assert(pins.AssignPending(7));
    assert(pins.CancelEmpty(c));pins.Queued(d);collect(7);
    c=pins.Reserve();assert(pins.Tag(c,9));assert(pins.Waiting());assert(pins.CancelEmpty(c));
    d=pins.Reserve();assert(pins.Pin(d,owner));assert(pins.Tag(d,11));assert(pins.AssignPending(11));
    synchronous=true;collect(11);assert(!pins.Waiting());
    c=pins.Reserve();assert(c!=Pins::Invalid);assert(pins.CancelEmpty(c));
    log.push_back(int(owner.use_count()));return log;
}
int main() {
    assert((Exercise<Native::Parallel::SyncOwnerPins<2,4>>() == Exercise<Reference::SyncOwnerPins<2,4>>()));
}
