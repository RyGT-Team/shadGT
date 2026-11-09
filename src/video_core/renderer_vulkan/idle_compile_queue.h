#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace Vulkan {
// Optional work never waits for queue space. Shutdown drains accepted callbacks
// with stopping=true so their completion promises and resource owners are released.
class IdleCompileQueue {
public:
    using Job = std::function<void(bool)>;
    explicit IdleCompileQueue(unsigned capacity, std::function<void()> initialize = {})
        : capacity{capacity}, worker{[this, initialize] {
            if (initialize) initialize();
            for (;;) {
                Job job;
                {
                    std::unique_lock lock{mutex};
                    cv.wait(lock, [this] { return stopping || !jobs.empty(); });
                    if (jobs.empty()) return;
                    job = std::move(jobs.front());
                    jobs.pop_front();
                }
                job(stopping.load());
            }
        }} {}
    ~IdleCompileQueue() {
        { std::scoped_lock lock{mutex}; stopping = true; }
        cv.notify_one();
        worker.join();
    }
    bool TryPush(Job job) {
        {
            std::unique_lock lock{mutex, std::try_to_lock};
            if (!lock || stopping || jobs.size() >= capacity) return false;
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
        return true;
    }
private:
    const unsigned capacity;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::atomic<bool> stopping{false};
    std::thread worker;
};
}
