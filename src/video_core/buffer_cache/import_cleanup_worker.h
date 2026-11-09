// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include "common/unique_function.h"

namespace VideoCore {
// Only GPU-complete batches may be handed here. Fence delivery merely enqueues;
// no driver destruction runs on the fence runner or under the queue mutex.
class ImportCleanupWorker {
public:
    explicit ImportCleanupWorker(std::function<void()> setup = {})
        : worker([this, setup = std::move(setup)] { if (setup) setup(); Run(); }) {}
    ~ImportCleanupWorker() {
        { std::scoped_lock lock(mutex); stopping = true; }
        cv.notify_one();
        if (worker.joinable()) worker.join();
    }
    void Enqueue(Common::UniqueFunction<void>&& callback) {
        { std::scoped_lock lock(mutex); pending.emplace_back(std::move(callback)); }
        cv.notify_one();
    }
    ImportCleanupWorker(const ImportCleanupWorker&) = delete;
    ImportCleanupWorker& operator=(const ImportCleanupWorker&) = delete;
private:
    void Run() {
        unsigned burst = 0;
        for (;;) {
            Common::UniqueFunction<void> callback;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return stopping || !pending.empty(); });
                if (pending.empty()) return;
                callback = std::move(pending.front());
                pending.pop_front();
            }
            callback();
            if (++burst == 8) { std::this_thread::yield(); burst = 0; }
        }
    }
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Common::UniqueFunction<void>> pending;
    bool stopping = false;
    std::thread worker;
};
} // namespace VideoCore
