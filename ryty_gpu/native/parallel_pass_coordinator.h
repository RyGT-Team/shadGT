#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <type_traits>
#include "ryty_gpu/native/ordered_pass_workers.h"
#include "ryty_gpu/native/pass_budget.h"
#include "ryty_gpu/native/parallel_pass_recording.h"
#include "ryty_gpu/native/rhi/rhi.h"
#include "ryty_gpu/native/rhi/completed_owner_release.h"
#include "ryty_gpu/native/completed_draw_vector_pool.h"

namespace Native::Parallel {
// One coordinator thread only. Worker callbacks record Vulkan commands only;
// they do not touch the renderer, registry, guest memory, or scheduler.
class PassCoordinator {
public:
    enum class RejectReason : std::size_t { Failed, ZeroEpoch, EpochMismatch, NodeCap, Validation, Invalid, Overflow, Charge, BudgetCount, BudgetBytes, Count };
    struct AdmissionDiag {
        u64 small_attempts{},small_accepted{},small_submit_failed{},prefix_attempts{},prefix_accepted{},prefix_exceptions{},large_attempts{},large_accepted{},large_submit_failed{},partial_drain{},partial_large{};
        std::array<u64,static_cast<std::size_t>(RejectReason::Count)> small_rejected{},prefix_rejected{},large_rejected{};
        PassBudget::Snapshot budget{};
    };
    using DrawPool=CompletedDrawVectorPool<Draw>;
    struct Job {
        Pass pass;
        std::optional<PassBudget::Token> credit;
        Rhi::CommandList* list{};
        bool recorded{};
        std::size_t loan=DrawPool::Invalid;
        Job(Pass p,PassBudget::Token token):pass(std::move(p)),credit(std::move(token)){}
        explicit Job(PassBudget::Token token):credit(std::move(token)){}
    };
    using JobPtr=std::shared_ptr<Job>;
    struct Group { std::vector<JobPtr> jobs; std::size_t draws{}; bool submitted{}, resolved{}, is_small{}; std::chrono::steady_clock::time_point submit_time{}; Group(){jobs.reserve(8);} };
    using GroupPtr=std::shared_ptr<Group>;
    struct PrefixLease {
        std::shared_ptr<const void> resources;
        std::optional<PassBudget::Token> credit;
        PrefixLease(std::shared_ptr<const void> owner,PassBudget::Token token):resources(std::move(owner)),credit(std::move(token)){}
    };
    using PrefixPtr=std::shared_ptr<PrefixLease>;
    struct Node {Rhi::CommandList* prefix{};JobPtr pass;PrefixPtr prefix_lease;GroupPtr group;bool resolved=false;std::function<void()> release;};
    struct InFlight {u64 tick{};JobPtr pass;PrefixPtr prefix_lease;std::function<void()> release;};
private:
    Rhi::Device& device_;
    const Vulkan::Instance& instance_;
    Rhi::Fence& timeline_;
    PassBudget budget_;
    std::shared_ptr<DrawPool> draw_pool_;
    OrderedPassWorkers<GroupPtr,GroupPtr> workers_;
    GroupPtr pending_small_;
    std::deque<Node> nodes_;
    std::size_t node_cap_;
    std::vector<InFlight> in_flight_;
    std::size_t in_flight_count_{};
    bool failed_{};
    std::atomic<u64> worker_jobs_{},worker_wall_ns_{},worker_cpu_100ns_{},worker_cpu_samples_{},worker_recorded_passes_{},worker_recorded_draws_{},worker_active_{},worker_peak_{};
    struct GroupStats { std::array<std::array<u64,9>,3> passes{},draws{}; u64 lead_samples{},lead_ns{},lead_max_ns{}; } group_stats_;
    u64 pending_epoch_{};
    u64 small_groups_submitted_{},small_groups_synchronous_{},small_passes_{},small_submitted_draws_{};
    u64 transfer_success_{},transfer_fallback_{};
    AdmissionDiag admission_diag_{};
    JobPtr Record(const JobPtr& job);
    std::function<void()> MakeRelease(const JobPtr& job);
    bool SubmitGroup(const GroupPtr& group);
    void CloseSmall(unsigned reason) noexcept;
    bool ResolveGroup(const GroupPtr& group, bool record_missing);
    RejectReason PrefixStateRejection(u64 epoch,Rhi::CommandList* list,bool has_resources) const noexcept;
    static void DropGroupJob(const GroupPtr& group,const JobPtr& job) noexcept;
public:
    PassCoordinator(Rhi::Device& device,const Vulkan::Instance& instance,Rhi::Fence& timeline,
                    std::size_t worker_count,std::size_t count_cap,std::size_t byte_cap,std::size_t node_cap);
    ~PassCoordinator();
    // Owner only: do not call concurrently with shutdown or retain the handles.
    template<class F> void ForEachWorkerHandle(F&& callback) {
        workers_.ForEachWorkerHandle(std::forward<F>(callback));
    }
    // Caller closes prefix and owns its list until Drain transfers it.
    bool PrefixPreflight(u64 epoch,Rhi::CommandList* list,const std::shared_ptr<const void>& resources,std::size_t total_owned_bytes);
    bool Prefix(u64 epoch,Rhi::CommandList* list,std::shared_ptr<const void> resources,std::size_t total_owned_bytes);
    bool TryPass(u64 epoch,const Pass& pass,std::size_t total_owned_bytes);
    bool TryPassTransfer(u64 epoch,Pass& pass,std::size_t total_owned_bytes);
    bool TrySmallPass(u64 epoch,const Pass& pass,std::size_t total_owned_bytes);
    // The first phase resolves every worker and fallback before touching the
    // primary. The caller must establish epoch and pre-reserve no-throw GPU
    // retirement storage before the accept phase. Accepted jobs and prefix
    // leases must remain retained through the corresponding GPU fence.
    template<class Accept> bool Drain(u64 signal_tick,Accept&& accept) {
        static_assert(std::is_nothrow_invocable_v<Accept,Rhi::CommandList*,JobPtr,PrefixPtr>, "accept must be noexcept");
        if(failed_ || !signal_tick || (pending_epoch_ && pending_epoch_!=signal_tick) || nodes_.size()+in_flight_count_>node_cap_)return false;
        CloseSmall(0); // submission/drain boundary
        for(auto& node:nodes_) {
            if(node.prefix) continue;
            if(!ResolveGroup(node.group,true))return false;
            if(!node.pass->recorded || !node.pass->list)return false;
        }
        while(!nodes_.empty()) {
            auto& node=nodes_.front();
            auto slot=std::find_if(in_flight_.begin(),in_flight_.end(),[](const InFlight& f){return f.tick==0;});
            if(slot==in_flight_.end()){failed_=true;return false;}
            // Fixed-cap slot retains owners and credits BEFORE primary recording.
            slot->tick=signal_tick;slot->pass=node.pass;slot->prefix_lease=node.prefix_lease;slot->release=std::move(node.release);++in_flight_count_;
            accept(node.prefix ? node.prefix : node.pass->list,node.pass,node.prefix_lease);
            DropGroupJob(node.group,node.pass);
            nodes_.pop_front();
        }
        pending_epoch_=0;
        return true;
    }
    void Collect(u64 completed_tick) noexcept {
        for(auto& slot:in_flight_) if(slot.tick && slot.tick<=completed_tick) {
            if(slot.release) {
                // release holds a Job copy before the slot drops its copy. The
                // worker owns the final release even if it runs immediately.
                slot.pass.reset();
                if(!Rhi::TryEnqueueCompleted(device_,slot.release))continue;
            }
            slot.prefix_lease.reset();slot.tick=0;--in_flight_count_;
        }
    }
    struct WorkerStats {u64 jobs,wall_ns,cpu_100ns,cpu_samples,peak,recorded_passes,recorded_draws,small_groups_submitted,small_groups_synchronous,small_passes,small_submitted_draws;};
    struct TransferStats {u64 success,fallback;};
    AdmissionDiag TakeAdmissionDiag() noexcept {auto result=admission_diag_;admission_diag_={};result.budget=budget_.TakeSnapshot();return result;}
    TransferStats TakeTransferStats() noexcept {return {std::exchange(transfer_success_,0),std::exchange(transfer_fallback_,0)};}
    bool TransferEnabled() const noexcept {return bool(draw_pool_);}
    WorkerStats TakeStats() noexcept {return {worker_jobs_.exchange(0),worker_wall_ns_.exchange(0),worker_cpu_100ns_.exchange(0),worker_cpu_samples_.exchange(0),worker_peak_.exchange(worker_active_.load()),worker_recorded_passes_.exchange(0),worker_recorded_draws_.exchange(0),std::exchange(small_groups_submitted_,0),std::exchange(small_groups_synchronous_,0),std::exchange(small_passes_,0),std::exchange(small_submitted_draws_,0)};}
    GroupStats TakeGroupStats() noexcept {auto result=group_stats_;group_stats_={};return result;}
    std::size_t PendingNodes() const noexcept {return nodes_.size();}
    std::size_t InFlightNodes() const noexcept {return in_flight_count_;}
    // A queued pass needs both a prefix node and a pass node. This is only
    // a snapshot: capacity may return before the current scope ends.
    bool HasRoomForPass() const noexcept {
        const auto used=nodes_.size()+in_flight_count_;
        return used<=node_cap_ && node_cap_-used>=2;
    }
};
}
