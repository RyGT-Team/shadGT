#pragma once
#include <condition_variable>
#include <cstddef>
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
    void Worker(std::size_t index) {
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
    OrderedPassWorkers(std::size_t count,std::size_t limit):limit_(limit) {
        if(!count || !limit) throw std::invalid_argument("zero worker count or limit");
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
