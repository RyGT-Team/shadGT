#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace Vulkan {
// Enqueue only after GPU completion. Driver destruction never holds the queue mutex.
class PipelineCleanupWorker {
public:
    explicit PipelineCleanupWorker(std::function<void()> setup = {})
        : worker([this, setup = std::move(setup)] { if (setup) setup(); Run(); }) {}
    ~PipelineCleanupWorker() {
        { std::lock_guard lock(mutex); stopping = true; }
        ready.notify_one();
        worker.join();
    }
    void Enqueue(std::function<void()> callback) {
        { std::lock_guard lock(mutex); pending.emplace_back(std::move(callback)); ++outstanding; }
        ready.notify_one();
    }
    size_t PendingCount() {
        std::lock_guard lock(mutex);
        return outstanding;
    }
    // Shutdown only: device and pipeline dependencies must remain alive until drained.
    void Drain() {
        std::unique_lock lock(mutex);
        idle.wait(lock, [this] { return outstanding == 0; });
    }
private:
    void Run() {
        unsigned burst = 0;
        for (;;) {
            std::function<void()> callback;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [this] { return stopping || !pending.empty(); });
                if (pending.empty()) return;
                callback = std::move(pending.front()); pending.pop_front();
            }
            callback();
            callback = {}; // Release captured pipeline before declaring the job complete.
            {
                std::lock_guard lock(mutex);
                if (--outstanding == 0) idle.notify_all();
            }
            if (++burst == 8) { std::this_thread::yield(); burst = 0; }
        }
    }
    std::mutex mutex;
    std::condition_variable ready, idle;
    std::deque<std::function<void()>> pending;
    size_t outstanding = 0;
    bool stopping = false;
    std::thread worker;
};
}
