#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
using AffinityMask = DWORD_PTR;
#else
using AffinityMask = uintptr_t;
#endif

// Isolated prototype. Single coordinator calls Submit/Take in order. The callback
// gets a worker index and immutable packet; it must own its worker's recording
// resources and return an object that keeps all GPU resources alive to retirement.
// This component does not classify passes or execute Vulkan commands.
template<class Packet, class Result>
class OrderedPassWorkers {
    using Task=std::packaged_task<Result(std::size_t)>;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Task> queue_;
    std::deque<std::future<Result>> ordered_;
    std::vector<std::thread> workers_;
    const std::size_t limit_;
    bool stop_=false;
    std::vector<AffinityMask> worker_affinity_masks_;
    void Worker(std::size_t index) {
        // Apply CPU affinity if configured for this worker
        if (index < worker_affinity_masks_.size() && worker_affinity_masks_[index] != 0) {
#ifdef _WIN32
            if (!::SetThreadAffinityMask(::GetCurrentThread(), static_cast<DWORD_PTR>(worker_affinity_masks_[index]))) {
                DWORD error = ::GetLastError();
                // Self-contained error reporting for standalone test compatibility
                fprintf(stderr, "Worker %zu: failed to set CPU affinity mask 0x%llx (Windows error %lu)\n",
                    index, static_cast<unsigned long long>(worker_affinity_masks_[index]), static_cast<unsigned long>(error));
                fflush(stderr);
            }
#else
            // On non-Windows, AffinityMask is uintptr_t but actual affinity setting
            // would use pthread_setaffinity_np with cpu_set_t; omitted for brevity
#endif
        }
        for (;;) {
            Task task;
            { std::unique_lock lock(mutex_); wake_.wait(lock,[&]{return stop_ || !queue_.empty();});
              if(queue_.empty()) { if(stop_) return; continue; }
              task=std::move(queue_.front());queue_.pop_front(); }
            task(index);
        }
    }
public:
    using Record=std::function<Result(std::size_t,const Packet&)>;
    OrderedPassWorkers(std::size_t count,std::size_t limit, const std::vector<AffinityMask>& worker_affinity_masks = {}):limit_(limit)
        , worker_affinity_masks_(worker_affinity_masks)
    {
        if(!count || !limit) throw std::invalid_argument("zero worker count or limit");
        if (!worker_affinity_masks_.empty() && worker_affinity_masks_.size() != count) {
            throw std::invalid_argument("worker_affinity_masks size must match worker count");
        }
        try {for(std::size_t i=0;i<count;++i) workers_.emplace_back([this,i]{Worker(i);});}
        catch(...) {{std::lock_guard lock(mutex_);stop_=true;}wake_.notify_all();for(auto& w:workers_)w.join();throw;}
    }
    OrderedPassWorkers(const OrderedPassWorkers&)=delete;
    OrderedPassWorkers& operator=(const OrderedPassWorkers&)=delete;
    void Shutdown() noexcept {
        {std::lock_guard lock(mutex_);stop_=true;}wake_.notify_all();
        for(auto& w:workers_)if(w.joinable())w.join();
    }
    ~OrderedPassWorkers() { Shutdown(); }
    // Owner only: do not call concurrently with Shutdown or retain the handles.
    template<class F> void ForEachWorkerHandle(F&& callback) {
        for(auto& worker:workers_) if(worker.joinable()) callback(worker.native_handle());
    }
    bool Submit(Packet packet,Record record) {
        if(!record) return false;

        Task task([packet=std::move(packet),record=std::move(record)](std::size_t worker){return record(worker,packet);});
        auto result=task.get_future();
        {std::lock_guard lock(mutex_); if(stop_ || ordered_.size()>=limit_)return false;
         ordered_.push_back(std::move(result));
         try {queue_.push_back(std::move(task));} catch(...) {ordered_.pop_back();throw;}}
        wake_.notify_one();return true;
    }
    std::optional<Result> Take() {
        std::future<Result> f;
        {std::lock_guard lock(mutex_);if(ordered_.empty())return std::nullopt;f=std::move(ordered_.front());ordered_.pop_front();}
        return f.get();
    }
};
