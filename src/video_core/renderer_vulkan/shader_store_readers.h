// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>
#include "common/types.h"

namespace Vulkan {
// Dedicated shader-store listing workers. The listing becomes available before
// optional binary prefetch; destruction stops and joins all workers.
template<class Listing> class ShaderStoreReaders {
public:
    using Read = std::function<Listing(u64)>;
    using Prefetch = std::function<void(u64, size_t)>;
    using Init = std::function<void(u32)>;
    ShaderStoreReaders(u32 count, Read read_, Prefetch prefetch_, Init init_ = {})
        : read{std::move(read_)}, prefetch{std::move(prefetch_)}, init{std::move(init_)} {
        for (u32 i=0; i<count; ++i) threads.emplace_back([this,i](std::stop_token stop) { Run(stop,i); });
    }
    ~ShaderStoreReaders() {
        for (auto& thread : threads) thread.request_stop();
        cv.notify_all();
    }
    std::future<Listing> List(u64 hash) {
        Job job{.hash=hash};
        auto result=job.promise.get_future();
        { std::scoped_lock lock{mutex}; queue.push_back(std::move(job)); }
        cv.notify_one();
        return result;
    }
private:
    struct Job { u64 hash{}; std::promise<Listing> promise; };
    void Run(std::stop_token stop, u32 index) {
        if (init) { try { init(index); } catch (...) {} }
        while (!stop.stop_requested()) {
            Job job;
            { std::unique_lock lock{mutex};
              cv.wait(lock,stop,[this] { return !queue.empty(); });
              if (queue.empty()) continue;
              job=std::move(queue.front()); queue.pop_front(); }
            try {
                auto listing=read(job.hash);
                std::vector<size_t> indices;
                for (const auto& entry : listing.entries)
                    if (entry.has_spv && indices.size()<8) indices.push_back(entry.perm_idx);
                job.promise.set_value(std::move(listing));
                for (const auto index : indices) {
                    if (stop.stop_requested()) break;
                    prefetch(job.hash,index);
                }
            } catch (...) {
                try { job.promise.set_exception(std::current_exception()); } catch (...) {}
            }
        }
    }
    Read read;
    Prefetch prefetch;
    Init init;
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<Job> queue;
    std::vector<std::jthread> threads;
};
}
