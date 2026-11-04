#include "ryty_gpu/native/parallel_pass_coordinator.h"
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#endif
#ifndef NDEBUG
#define COORD_DIAG(...) __VA_ARGS__
#else
#define COORD_DIAG(...)
#endif

namespace Native::Parallel {
namespace {
bool GateDiag() noexcept {static const bool on=[] {auto p=std::getenv("RYGT_PARALLEL_GATE_DIAG");return p && p[0]=='1' && !p[1];}();return on;}
bool BatchDiag() noexcept {static const bool on=[] {auto p=std::getenv("RYGT_PARALLEL_BATCH_DIAG");return p && p[0]=='1' && !p[1];}();return on;}
bool BatchTwoPass() noexcept {static const bool on=[] {auto p=std::getenv("RYGT_PARALLEL_BATCH_TWO_PASSES");return p && p[0]=='1' && !p[1];}();return on;}
bool TransferPasses() noexcept {static const bool on=[] {auto p=std::getenv("RYGT_PARALLEL_TRANSFER_PASSES");return p && p[0]=='1' && !p[1];}();return on;}
bool ThreadCpu100ns(u64& out) noexcept {
#ifdef _WIN32
    FILETIME create{},exit{},kernel{},user{};
    if(GetThreadTimes(GetCurrentThread(),&create,&exit,&kernel,&user)) {
        out=(u64(kernel.dwHighDateTime)<<32 | kernel.dwLowDateTime)+(u64(user.dwHighDateTime)<<32 | user.dwLowDateTime);return true;
    }
#endif
    return false;
}
}
PassCoordinator::PassCoordinator(Rhi::Device& device,const Vulkan::Instance& instance,Rhi::Fence& timeline,
    std::size_t worker_count,std::size_t count_cap,std::size_t byte_cap,std::size_t node_cap)
    // Prefixes and pass jobs each retain a credit; bound those credits by the
    // existing node limit while keeping the worker queue's separate limit.
    :device_(device),instance_(instance),timeline_(timeline),budget_(node_cap,byte_cap),workers_(worker_count,count_cap),node_cap_(node_cap),in_flight_(node_cap) {
    if(TransferPasses())try {draw_pool_=std::make_shared<DrawPool>();} catch(...) {}
}
PassCoordinator::~PassCoordinator() {
    // The owner must first prevent new producers and ensure no accepted node has
    // been submitted; transfer/retirement of accepted nodes belongs to Drain.
    // Join every submitted task even after a future mismatch or failed drain.
    // No worker may still mutate a Job when its unsubmitted list is released.
    workers_.Shutdown();
    pending_small_.reset();
    while(!nodes_.empty()) {
        Node node=std::move(nodes_.front());nodes_.pop_front();
        if(node.prefix) {device_.ReleaseUnsubmitted(node.prefix);continue;}
        if(node.pass && node.pass->list){device_.ReleaseUnsubmitted(node.pass->list);node.pass->list=nullptr;}
        DropGroupJob(node.group,node.pass);
        if(node.release) {
            node.pass.reset(); // callback retains the Job until the worker releases it
            while(!Rhi::TryEnqueueCompleted(device_,node.release))std::this_thread::yield();
        }
    }
    // The owner must have submitted every accepted node before destruction.
    // Never release an in-flight lease before its actual GPU timeline signals.
    u64 last=0;for(const auto& node:in_flight_)last=last<node.tick?node.tick:last;
    if(last && (!timeline_.Wait(last,~u64{0}) || timeline_.Completed()<last))std::terminate();
    while(in_flight_count_) {Collect(timeline_.Completed());if(in_flight_count_)std::this_thread::yield();}
}
PassCoordinator::JobPtr PassCoordinator::Record(const JobPtr& job) {
    Rhi::CommandList* list=nullptr;
    try {
        list=device_.AcquireCommandList(Rhi::CommandListLevel::Secondary);
        if(!list) return job;
        list->Begin();
        const auto cmd=vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(list->NativeHandle())};
        if(!RecordPass(cmd,instance_,job->pass)) {device_.ReleaseUnsubmitted(list);return job;}
        list->End();job->list=list;job->recorded=true;
    } catch(...) {if(list)device_.ReleaseUnsubmitted(list);}
    return job;
}
std::function<void()> PassCoordinator::MakeRelease(const JobPtr& job) {
    if(!draw_pool_)return [owner=job]{};
    return [owner=job,pool=draw_pool_] {
        auto& draws=owner->pass.draws;
        if(owner->loan!=DrawPool::Invalid) {
            draws.clear();
            // A mismatched loan is retained as charged/unavailable; the
            // vector is disposed on this worker rather than reused early.
            (void)pool->ReturnLoan(owner->loan,draws);
        } else (void)pool->Seed(draws);
    };
}
PassCoordinator::RejectReason PassCoordinator::PrefixStateRejection(u64 epoch,Rhi::CommandList* list,bool has_resources) const noexcept {
    if(failed_)return RejectReason::Failed;
    if(!epoch)return RejectReason::ZeroEpoch;
    if(pending_epoch_ && pending_epoch_!=epoch)return RejectReason::EpochMismatch;
    if(!list || !has_resources)return RejectReason::Invalid;
    if(nodes_.size()+in_flight_count_>=node_cap_)return RejectReason::NodeCap;
    return RejectReason::Count;
}
bool PassCoordinator::PrefixPreflight(u64 epoch,Rhi::CommandList* list,const std::shared_ptr<const void>& resources,std::size_t total_owned_bytes) {
    auto why=PrefixStateRejection(epoch,list,bool(resources));
    if(why==RejectReason::Count) {
        const auto budget=budget_.CheckAcquire(total_owned_bytes);
        if(budget==PassBudget::Rejection::Count)why=RejectReason::BudgetCount;
        else if(budget==PassBudget::Rejection::Bytes)why=RejectReason::BudgetBytes;
    }
    if(why==RejectReason::Count)return true;
    COORD_DIAG(++admission_diag_.prefix_attempts);COORD_DIAG(++admission_diag_.prefix_rejected[static_cast<std::size_t>(why)]);return false;
}
bool PassCoordinator::Prefix(u64 epoch,Rhi::CommandList* list,std::shared_ptr<const void> resources,std::size_t total_owned_bytes) {
    COORD_DIAG(++admission_diag_.prefix_attempts);
    const auto reject=[&](RejectReason why){COORD_DIAG(++admission_diag_.prefix_rejected[static_cast<std::size_t>(why)]);return false;};
    const auto state=PrefixStateRejection(epoch,list,bool(resources));
    if(state!=RejectReason::Count)return reject(state);
    PassBudget::Rejection reason{};auto credit=budget_.TryAcquire(total_owned_bytes,&reason);
    if(!credit)return reject(reason==PassBudget::Rejection::Count?RejectReason::BudgetCount:RejectReason::BudgetBytes);
    try {
        auto lease=std::make_shared<PrefixLease>(std::move(resources),std::move(*credit));
        nodes_.push_back({list,{},std::move(lease),{},true});pending_epoch_=epoch;COORD_DIAG(++admission_diag_.prefix_accepted);return true;
    } catch(...) {COORD_DIAG(++admission_diag_.prefix_exceptions);throw;}
}
void PassCoordinator::CloseSmall(unsigned reason) noexcept {
    if(pending_small_ && !pending_small_->submitted) {
        if(reason==0)COORD_DIAG(++admission_diag_.partial_drain);
        else if(reason==1)COORD_DIAG(++admission_diag_.partial_large);
    }
#ifndef NDEBUG
    if(pending_small_ && BatchDiag() && reason<3) {
        ++group_stats_.passes[reason][std::min<std::size_t>(8,pending_small_->jobs.size())];
        ++group_stats_.draws[reason][std::min<std::size_t>(8,pending_small_->draws)];
    }
#endif
    pending_small_.reset();
}
void PassCoordinator::DropGroupJob(const GroupPtr& group,const JobPtr& job) noexcept {
    if(group) for(auto& entry:group->jobs) if(entry==job){entry.reset();break;}
}
bool PassCoordinator::ResolveGroup(const GroupPtr& group,bool record_missing) {
    if(!group || failed_)return false;
    if(!group->resolved) {
        if(group->submitted) {
#ifndef NDEBUG
            if(group->is_small && BatchDiag()) {auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-group->submit_time).count();if(ns>=0){++group_stats_.lead_samples;group_stats_.lead_ns+=u64(ns);group_stats_.lead_max_ns=(std::max)(group_stats_.lead_max_ns,u64(ns));}}
#endif
            try {auto result=workers_.Take();if(!result || *result!=group){failed_=true;return false;}}
            catch(...) { /* completed task failed; recover each unfinished pass below */ }
        }
        group->resolved=true;
        if(record_missing && !group->submitted && group->is_small)COORD_DIAG(++small_groups_synchronous_);
    }
    if(record_missing) for(auto& job:group->jobs) { if(job && !job->recorded) Record(job); if(job && (!job->recorded || !job->list))return false; }
    return true;
}
bool PassCoordinator::SubmitGroup(const GroupPtr& group) {
    try {
#ifndef NDEBUG
        if(group->is_small && BatchDiag())group->submit_time=std::chrono::steady_clock::now();
#endif
        bool ok=workers_.Submit(group,[this](std::size_t,const GroupPtr& task){
#ifndef NDEBUG
            const u64 active=worker_active_.fetch_add(1)+1;u64 peak=worker_peak_.load();
            while(peak<active && !worker_peak_.compare_exchange_weak(peak,active)) {}
            u64 cpu_start=0,cpu_end=0;const bool cpu_started=GateDiag() && ThreadCpu100ns(cpu_start);
            const auto start=std::chrono::steady_clock::now();
            u64 recorded_passes=0,recorded_draws=0;
            for(const auto& job:task->jobs) { Record(job); if(job->recorded){++recorded_passes;recorded_draws+=job->pass.draws.size();} }
            worker_recorded_passes_.fetch_add(recorded_passes);worker_recorded_draws_.fetch_add(recorded_draws);
            if(cpu_started && ThreadCpu100ns(cpu_end) && cpu_end>=cpu_start){worker_cpu_100ns_.fetch_add(cpu_end-cpu_start);worker_cpu_samples_.fetch_add(1);}
            worker_wall_ns_.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());
            worker_jobs_.fetch_add(1);worker_active_.fetch_sub(1);
#else
            for(const auto& job:task->jobs) Record(job);
#endif
            return task;
        });
        group->submitted=ok;return ok;
    } catch(...) {return false;}
}
bool PassCoordinator::TryPass(u64 epoch,const Pass& pass,std::size_t total_owned_bytes) {
    COORD_DIAG(++admission_diag_.large_attempts);
    const auto reject=[&](RejectReason why){COORD_DIAG(++admission_diag_.large_rejected[static_cast<std::size_t>(why)]);return false;};
    // A later task must not overtake an earlier, incomplete group in the future queue.
    CloseSmall(1); // larger pass splits a pending group before admission
    if(failed_)return reject(RejectReason::Failed);
    if(!epoch)return reject(RejectReason::ZeroEpoch);
    if(pending_epoch_ && pending_epoch_!=epoch)return reject(RejectReason::EpochMismatch);
    if(nodes_.size()+in_flight_count_>=node_cap_)return reject(RejectReason::NodeCap);
    if(!ValidatePass(pass))return reject(RejectReason::Validation);
    if(pass.draws.capacity()>(~std::size_t{0}-sizeof(Pass))/sizeof(Draw))return reject(RejectReason::Overflow);
    if(total_owned_bytes<sizeof(Pass)+pass.draws.capacity()*sizeof(Draw))return reject(RejectReason::Charge);
    auto group=std::make_shared<Group>();
    if(group->jobs.capacity()>(~std::size_t{0}-sizeof(Group))/sizeof(JobPtr))return reject(RejectReason::Overflow);
    const auto overhead=sizeof(Group)+group->jobs.capacity()*sizeof(JobPtr);
    if(total_owned_bytes>~std::size_t{0}-overhead)return reject(RejectReason::Overflow);
    PassBudget::Rejection reason{};auto credit=budget_.TryAcquire(total_owned_bytes+overhead,&reason);if(!credit)return reject(reason==PassBudget::Rejection::Count?RejectReason::BudgetCount:RejectReason::BudgetBytes);
    auto job=std::make_shared<Job>(pass,std::move(*credit));
    group->jobs.push_back(job);group->draws=pass.draws.size();
    std::function<void()> release=MakeRelease(job);
    nodes_.push_back({nullptr,job,{},group,false,std::move(release)});
    if(!SubmitGroup(group)){nodes_.pop_back();COORD_DIAG(++admission_diag_.large_submit_failed);return false;}
    pending_epoch_=epoch;COORD_DIAG(++admission_diag_.large_accepted);return true;
}
bool PassCoordinator::TryPassTransfer(u64 epoch,Pass& pass,std::size_t total_owned_bytes) {
    if(!draw_pool_)return TryPass(epoch,pass,total_owned_bytes);
    struct Loan {
        DrawPool* pool;std::size_t index;std::vector<Draw> spare;
        ~Loan(){if(index!=DrawPool::Invalid && !pool->ReturnLoan(index,spare))std::terminate();}
    } loan{draw_pool_.get(),DrawPool::Invalid,{}};
    loan.index=draw_pool_->TryTake(loan.spare,pass.draws.capacity());
    if(loan.index==DrawPool::Invalid){COORD_DIAG(++transfer_fallback_);return TryPass(epoch,pass,total_owned_bytes);}
    COORD_DIAG(++admission_diag_.large_attempts);
    const auto reject=[&](RejectReason why){COORD_DIAG(++admission_diag_.large_rejected[static_cast<std::size_t>(why)]);return false;};
    CloseSmall(1);
    if(failed_)return reject(RejectReason::Failed);
    if(!epoch)return reject(RejectReason::ZeroEpoch);
    if(pending_epoch_ && pending_epoch_!=epoch)return reject(RejectReason::EpochMismatch);
    if(nodes_.size()+in_flight_count_>=node_cap_)return reject(RejectReason::NodeCap);
    if(!ValidatePass(pass))return reject(RejectReason::Validation);
    if(pass.draws.capacity()>(~std::size_t{0}-sizeof(Pass))/sizeof(Draw))return reject(RejectReason::Overflow);
    if(total_owned_bytes<sizeof(Pass)+pass.draws.capacity()*sizeof(Draw))return reject(RejectReason::Charge);
    auto group=std::make_shared<Group>();
    if(group->jobs.capacity()>(~std::size_t{0}-sizeof(Group))/sizeof(JobPtr))return reject(RejectReason::Overflow);
    const auto overhead=sizeof(Group)+group->jobs.capacity()*sizeof(JobPtr);
    if(total_owned_bytes>~std::size_t{0}-overhead)return reject(RejectReason::Overflow);
    PassBudget::Rejection reason{};auto credit=budget_.TryAcquire(total_owned_bytes+overhead,&reason);if(!credit)return reject(reason==PassBudget::Rejection::Count?RejectReason::BudgetCount:RejectReason::BudgetBytes);
    auto job=std::make_shared<Job>(std::move(*credit));
    job->pass.target=pass.target;job->pass.extended_verified=pass.extended_verified;job->pass.resource_lease=pass.resource_lease;
    group->jobs.push_back(job);group->draws=pass.draws.size();
    std::function<void()> release=MakeRelease(job);
    nodes_.push_back({nullptr,job,{},group,false,std::move(release)});
    job->loan=loan.index;
    job->pass.draws.swap(pass.draws);pass.draws.swap(loan.spare);
    if(!SubmitGroup(group)) {
        pass.draws.swap(loan.spare);job->pass.draws.swap(pass.draws);job->loan=DrawPool::Invalid;
        nodes_.pop_back();COORD_DIAG(++admission_diag_.large_submit_failed);return false; // loan destructor restores the spare
    }
    loan.index=DrawPool::Invalid;COORD_DIAG(++transfer_success_);pending_epoch_=epoch;COORD_DIAG(++admission_diag_.large_accepted);return true;
}
bool PassCoordinator::TrySmallPass(u64 epoch,const Pass& pass,std::size_t total_owned_bytes) {
    COORD_DIAG(++admission_diag_.small_attempts);
    const auto reject=[&](RejectReason why){COORD_DIAG(++admission_diag_.small_rejected[static_cast<std::size_t>(why)]);return false;};
    if(failed_)return reject(RejectReason::Failed);
    if(!epoch)return reject(RejectReason::ZeroEpoch);
    if(pending_epoch_ && pending_epoch_!=epoch)return reject(RejectReason::EpochMismatch);
    if(nodes_.size()+in_flight_count_>=node_cap_)return reject(RejectReason::NodeCap);
    if(!ValidatePass(pass))return reject(RejectReason::Validation);
    if(pass.draws.empty() || pass.draws.size()>=8)return reject(RejectReason::Invalid);
    if(pass.draws.capacity()>(~std::size_t{0}-sizeof(Pass))/sizeof(Draw))return reject(RejectReason::Overflow);
    if(total_owned_bytes<sizeof(Pass)+pass.draws.capacity()*sizeof(Draw))return reject(RejectReason::Charge);
    auto group=pending_small_;
    if(!group) {
        group=std::make_shared<Group>();group->is_small=true;
        if(group->jobs.capacity()>(~std::size_t{0}-sizeof(Group))/sizeof(JobPtr))return reject(RejectReason::Overflow);
        const auto overhead=sizeof(Group)+group->jobs.capacity()*sizeof(JobPtr);
        if(total_owned_bytes>~std::size_t{0}-overhead)return reject(RejectReason::Overflow);
        total_owned_bytes+=overhead;
    }
    PassBudget::Rejection reason{};auto credit=budget_.TryAcquire(total_owned_bytes,&reason);
    if(!credit)return reject(reason==PassBudget::Rejection::Count?RejectReason::BudgetCount:RejectReason::BudgetBytes);
    auto job=std::make_shared<Job>(pass,std::move(*credit));
    std::function<void()> release=MakeRelease(job);
    group->jobs.push_back(job); // Group constructor pre-reserves all eight entries.
    try {nodes_.push_back({nullptr,job,{},group,false,std::move(release)});}
    catch(...) {group->jobs.pop_back();throw;}
    group->draws+=pass.draws.size();COORD_DIAG(++small_passes_);pending_epoch_=epoch;pending_small_=group;
    if(group->draws>=8 || group->jobs.size()==8 || (BatchTwoPass() && group->jobs.size()>=2)) {
        // Once published, a failed submission remains an ordered synchronous group.
        if(SubmitGroup(group)){COORD_DIAG(++small_groups_submitted_);COORD_DIAG(small_submitted_draws_+=group->draws);}else COORD_DIAG(++admission_diag_.small_submit_failed);CloseSmall(2);
    }
    COORD_DIAG(++admission_diag_.small_accepted);return true;
}
}

#undef COORD_DIAG
