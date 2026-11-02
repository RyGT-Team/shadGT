#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
namespace Native::Parallel {
#ifdef RYGT_PROOF_TEST_HOOK
void ProofGuardTestHook(int stage);
#endif
struct ProofProducerContext {
    // Low bit is enabled; upper bits are a monotonically advancing mode generation.
    std::atomic<std::uint64_t> state{0},active{0};
    std::atomic<bool> closing{false};
    void SetMode(bool enabled) noexcept {
        auto old=state.load(std::memory_order_seq_cst);
        for(;;) {if(bool(old&1)==enabled)return;const auto next=((old>>1)+1)*2+enabled;
            if(state.compare_exchange_weak(old,next,std::memory_order_seq_cst))return;}
    }
    bool Valid(std::uint64_t s) const noexcept {
        return (s&1) && !closing.load(std::memory_order_acquire) && state.load(std::memory_order_seq_cst)==s;
    }
};
struct ProofProducerGuard {
    std::shared_ptr<ProofProducerContext> context;std::uint64_t state{};
    ProofProducerGuard()=default;
    explicit ProofProducerGuard(std::shared_ptr<ProofProducerContext> p):context(std::move(p)) {
        if(!context)return;state=context->state.load(std::memory_order_seq_cst);
        if(!context->Valid(state)){context.reset();return;}
#ifdef RYGT_PROOF_TEST_HOOK
        ProofGuardTestHook(1);
#endif
        context->active.fetch_add(1,std::memory_order_seq_cst);
#ifdef RYGT_PROOF_TEST_HOOK
        ProofGuardTestHook(2);
#endif
        if(!context->Valid(state)){context->active.fetch_sub(1,std::memory_order_seq_cst);context.reset();}
    }
    ProofProducerGuard(const ProofProducerGuard&)=delete;
    ProofProducerGuard& operator=(const ProofProducerGuard&)=delete;
    ProofProducerGuard(ProofProducerGuard&& other) noexcept:context(std::move(other.context)),state(other.state){}
    ~ProofProducerGuard(){if(context)context->active.fetch_sub(1,std::memory_order_seq_cst);}
};
class ProofProducerRegistry {
    std::mutex mutex_;std::unordered_map<const void*,std::shared_ptr<ProofProducerContext>> map_;
    std::atomic<std::uint64_t> generation_{1};
public:
    void Register(const void* key) {auto c=std::make_shared<ProofProducerContext>();std::lock_guard lock(mutex_);auto it=map_.find(key);
        if(it!=map_.end()){it->second->closing.store(true,std::memory_order_release);it->second->SetMode(false);it->second=std::move(c);}
        else map_.emplace(key,std::move(c));generation_.fetch_add(1,std::memory_order_release);}
    void Close(const void* key) noexcept {std::lock_guard lock(mutex_);auto it=map_.find(key);if(it==map_.end())return;it->second->closing.store(true,std::memory_order_release);it->second->SetMode(false);map_.erase(it);generation_.fetch_add(1,std::memory_order_release);}
    std::shared_ptr<ProofProducerContext> FindBlocking(const void* key) {std::lock_guard lock(mutex_);auto i=map_.find(key);return i==map_.end()?nullptr:i->second;}
    ProofProducerGuard TryEnter(const void* key) noexcept {
        // One harmless strong context per producer thread avoids refcount work
        // in serial mode; closing/generation make a retained stale context inert.
        struct Cache {const ProofProducerRegistry* registry{};const void* key{};std::uint64_t generation{};std::shared_ptr<ProofProducerContext> context;};
        static thread_local Cache cache;
        const auto g=generation_.load(std::memory_order_acquire);
        if(cache.registry==this && cache.key==key && cache.generation==g) {
            if(!cache.context)return {};
            const auto s=cache.context->state.load(std::memory_order_seq_cst);
            if(!cache.context->Valid(s))return {};
            return ProofProducerGuard(cache.context);
        }
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock)return {};
        const auto i=map_.find(key);auto c=i==map_.end()?std::shared_ptr<ProofProducerContext>{}:i->second;
        cache={this,key,generation_.load(std::memory_order_relaxed),c};return ProofProducerGuard(std::move(c));
    }
};
}
