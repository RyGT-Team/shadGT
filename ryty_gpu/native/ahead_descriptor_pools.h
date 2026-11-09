#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace Native::Parallel {
// Bounded pool-local inventory of complete layout groups. Spares retain their
// pipeline owner, and are purged only after reset at a completed GPU epoch.
template<class Backend,std::size_t MaxBatches=16,std::size_t MaxSets=8,std::size_t MaxSpares=64> class AheadDescriptorPools {
public:
    using Pool=typename Backend::Pool;using Layout=typename Backend::Layout;using Set=typename Backend::Set;
    using Counts=std::array<std::size_t,4>;
private:
    struct Spare {std::array<Layout,MaxSets> layouts{};std::array<Set,MaxSets> sets{};Counts counts{};std::size_t count{};std::shared_ptr<const void> owner;};
    struct Batch {Pool pool{};std::atomic<std::size_t> live{};std::atomic<std::uint64_t> last_tick{};std::size_t allocated{};Counts descriptors{};bool sealed{},bad{};std::array<Spare,MaxSpares> spares{},seen{};std::size_t spare_count{},seen_count{};};
    struct State {
        Backend backend;std::mutex mutex;std::array<Batch,MaxBatches> batches{};std::size_t spare_count{};
        std::atomic<bool> enabled{false},closing{false};std::atomic<std::size_t> preparing{};
        std::atomic<std::uint64_t> calls{},sets_allocated{},spare_hits{},discarded{},purged{};
        explicit State(Backend b):backend(std::move(b)){}
    };
public:
    struct Lease {
        std::shared_ptr<State> owner;std::shared_ptr<const void> layout_owner;Batch* batch{};std::array<Set,MaxSets> sets{};std::size_t count{};
        Lease(std::shared_ptr<State> s,Batch* b,std::shared_ptr<const void> pin):owner(std::move(s)),layout_owner(std::move(pin)),batch(b){batch->live.fetch_add(1,std::memory_order_acq_rel);}
        Lease(const Lease&)=delete;Lease& operator=(const Lease&)=delete;
        ~Lease(){if(batch)batch->live.fetch_sub(1,std::memory_order_release);}
        bool MarkUsed(std::uint64_t tick) noexcept {if(!tick)return false;auto old=batch->last_tick.load(std::memory_order_relaxed);while(old<tick && !batch->last_tick.compare_exchange_weak(old,tick,std::memory_order_release,std::memory_order_relaxed)) {}return true;}
    };
    using LeasePtr=std::shared_ptr<Lease>;
    enum class Failure {None,Disabled,Invalid,Busy,Capacity,Driver};
    struct Stats {std::uint64_t calls,sets_allocated,spare_hits,discarded,purged;};
private:
    std::shared_ptr<State> state_;
    static bool Matches(const Spare& s,std::span<const Layout> layouts,const Counts& counts,const std::shared_ptr<const void>& owner) noexcept {
        if(s.owner.get()!=owner.get() || s.count!=layouts.size() || s.counts!=counts)return false;
        for(std::size_t i=0;i<s.count;++i)if(s.layouts[i]!=layouts[i])return false;return true;
    }
    static void ClearSpares(State& s,Batch& b) noexcept {
        s.purged.fetch_add(b.spare_count,std::memory_order_relaxed);s.spare_count-=b.spare_count;for(std::size_t i=0;i<b.spare_count;++i)b.spares[i]=Spare{};b.spare_count=0;for(std::size_t i=0;i<b.seen_count;++i)b.seen[i]=Spare{};b.seen_count=0;
    }
public:
    explicit AheadDescriptorPools(Backend backend):state_(std::make_shared<State>(std::move(backend))){}
    void SetEnabled(bool enabled) noexcept {state_->enabled.store(enabled,std::memory_order_release);}
    bool Quiescent()const noexcept{return state_->preparing.load(std::memory_order_acquire)==0;}
    void BeginClose()noexcept{state_->closing.store(true,std::memory_order_release);state_->enabled.store(false,std::memory_order_release);}
    Stats GetStats() const noexcept {return {state_->calls.load(std::memory_order_relaxed),state_->sets_allocated.load(std::memory_order_relaxed),state_->spare_hits.load(std::memory_order_relaxed),state_->discarded.load(std::memory_order_relaxed),state_->purged.load(std::memory_order_relaxed)};}
    LeasePtr TryPrepare(std::span<const Layout> layouts,const Counts& counts,std::shared_ptr<const void> layout_owner,std::uint64_t completed,Failure* reason=nullptr) noexcept {
        const auto fail=[&](Failure f){if(reason)*reason=f;return LeasePtr{};};if(reason)*reason=Failure::None;
        auto s=state_;if(layouts.empty() || layouts.size()>MaxSets || !layout_owner)return fail(Failure::Invalid);
        if(!s->enabled.load(std::memory_order_acquire) || s->closing.load(std::memory_order_acquire))return fail(Failure::Disabled);
        for(std::size_t i=0;i<counts.size();++i)if(counts[i]>Backend::MaxDescriptorsPerPool[i])return fail(Failure::Capacity);
        s->preparing.fetch_add(1,std::memory_order_acq_rel);
        struct Guard {State& s;~Guard(){s.preparing.fetch_sub(1,std::memory_order_release);}} guard{*s};
        if(!s->enabled.load(std::memory_order_acquire) || s->closing.load(std::memory_order_acquire))return fail(Failure::Disabled);
        try {std::unique_lock lock{s->mutex,std::try_to_lock};if(!lock)return fail(Failure::Busy);
            if(!s->enabled.load(std::memory_order_acquire) || s->closing.load(std::memory_order_acquire))return fail(Failure::Disabled);
            const auto fits=[&](const Batch& b){if(b.allocated+layouts.size()>Backend::MaxSetsPerPool)return false;for(std::size_t i=0;i<counts.size();++i)if(counts[i]>Backend::MaxDescriptorsPerPool[i]-b.descriptors[i])return false;return true;};
            // Check existing complete spares first, even in sealed pools.
            for(auto& b:s->batches)if(b.pool && !b.bad)for(std::size_t i=0;i<b.spare_count;++i)if(Matches(b.spares[i],layouts,counts,layout_owner)) {
                auto lease=std::make_shared<Lease>(s,&b,layout_owner);lease->count=layouts.size();lease->sets=b.spares[i].sets;
                b.spares[i]=std::move(b.spares[b.spare_count-1]);b.spares[b.spare_count-1]=Spare{};--b.spare_count;--s->spare_count;
                s->spare_hits.fetch_add(1,std::memory_order_relaxed);
                if(!s->enabled.load(std::memory_order_acquire) || s->closing.load(std::memory_order_acquire)){s->discarded.fetch_add(1,std::memory_order_relaxed);return fail(Failure::Disabled);}
                return lease;
            }
            Batch* chosen=nullptr;
            for(auto& b:s->batches){
                if(b.pool && !b.bad && (b.sealed || !fits(b)) && b.live.load(std::memory_order_acquire)==0 && b.last_tick.load(std::memory_order_acquire)<=completed){
                    if(!s->backend.Reset(b.pool)){b.bad=true;continue;}
                    ClearSpares(*s,b);b.allocated=0;b.descriptors={};b.sealed=false;b.last_tick.store(0,std::memory_order_relaxed);
                }
                if(b.pool && !b.bad && !b.sealed && fits(b)){chosen=&b;break;}
            }
            if(!chosen)for(auto& b:s->batches)if(!b.pool){b.pool=s->backend.Create();if(!b.pool)return fail(Failure::Driver);chosen=&b;break;}
            if(!chosen)return fail(Failure::Capacity);
            auto lease=std::make_shared<Lease>(s,chosen,layout_owner);lease->count=layouts.size();
            bool seen=false;for(std::size_t i=0;i<chosen->seen_count;++i)if(Matches(chosen->seen[i],layouts,counts,layout_owner)){seen=true;break;}
            std::size_t batches=seen?std::min<std::size_t>({32,(Backend::MaxSetsPerPool-chosen->allocated)/layouts.size(),1+MaxSpares-s->spare_count}):1;
            for(std::size_t i=0;i<counts.size();++i)if(counts[i])batches=std::min(batches,(Backend::MaxDescriptorsPerPool[i]-chosen->descriptors[i])/counts[i]);
            if(!batches)return fail(Failure::Capacity);
            std::array<Layout,32*MaxSets> alloc_layouts{};std::array<Set,32*MaxSets> allocated{};
            for(std::size_t j=0;j<batches;++j)for(std::size_t i=0;i<layouts.size();++i)alloc_layouts[j*layouts.size()+i]=layouts[i];
            auto allocate=[&](std::size_t groups){s->calls.fetch_add(1,std::memory_order_relaxed);return s->backend.Allocate(chosen->pool,std::span<const Layout>{alloc_layouts.data(),groups*layouts.size()},std::span<Set>{allocated.data(),groups*layouts.size()});};
            if(!allocate(batches)){if(batches==1){chosen->sealed=true;return fail(Failure::Driver);}batches=1;if(!allocate(1)){chosen->sealed=true;return fail(Failure::Driver);}}
            chosen->allocated+=batches*layouts.size();for(std::size_t i=0;i<counts.size();++i)chosen->descriptors[i]+=counts[i]*batches;
            s->sets_allocated.fetch_add(batches*layouts.size(),std::memory_order_relaxed);
            if(!seen && chosen->seen_count<MaxSpares){auto& item=chosen->seen[chosen->seen_count++];item.count=layouts.size();item.counts=counts;item.owner=layout_owner;for(std::size_t i=0;i<layouts.size();++i)item.layouts[i]=layouts[i];}
            for(std::size_t i=0;i<layouts.size();++i)lease->sets[i]=allocated[i];
            for(std::size_t j=1;j<batches;++j){auto& spare=chosen->spares[chosen->spare_count++];spare.count=layouts.size();spare.counts=counts;spare.owner=layout_owner;for(std::size_t i=0;i<layouts.size();++i){spare.layouts[i]=layouts[i];spare.sets[i]=allocated[j*layouts.size()+i];}++s->spare_count;}
            if(!s->enabled.load(std::memory_order_acquire) || s->closing.load(std::memory_order_acquire)){s->discarded.fetch_add(1,std::memory_order_relaxed);return fail(Failure::Disabled);}
            return lease;
        } catch(...) {return fail(Failure::Driver);}
    }
    bool DestroyReady(std::uint64_t completed) noexcept {
        auto s=state_;std::unique_lock lock{s->mutex,std::try_to_lock};if(!lock || !s->closing.load(std::memory_order_acquire) || s->preparing.load(std::memory_order_acquire))return false;
        for(auto& b:s->batches)if(b.live.load(std::memory_order_acquire) || b.last_tick.load(std::memory_order_acquire)>completed)return false;
        for(auto& b:s->batches)if(b.pool){s->backend.Destroy(b.pool);ClearSpares(*s,b);b.pool=Pool{};}return true;
    }
};
}
