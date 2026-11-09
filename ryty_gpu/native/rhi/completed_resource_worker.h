#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace Native::Rhi {

// Accept only callbacks whose GPU completion has already been established.
// A full or contended queue leaves ownership with the caller for a later pass.
class CompletedResourceWorker {
public:
    static constexpr size_t Capacity = 256;
    static constexpr size_t BurstLimit = 8;
    static constexpr auto BurstBudget = std::chrono::milliseconds{1};
    static constexpr auto YieldInterval = std::chrono::milliseconds{1};

    explicit CompletedResourceWorker(std::function<void()> setup = {})
        : worker{[this, setup = std::move(setup)] { if (setup) setup(); Run(); }} {}
    ~CompletedResourceWorker() { DrainAndStop(); }
    CompletedResourceWorker(const CompletedResourceWorker&) = delete;
    CompletedResourceWorker& operator=(const CompletedResourceWorker&) = delete;

    bool TryEnqueue(std::function<void()>& callback) {
        // Collect normally serializes producers, but allocation-pressure callers
        // may collect concurrently. Never wait for either producer or worker.
        std::unique_lock lock{producer_mutex, std::try_to_lock};
        if (!lock || stopping.load(std::memory_order_acquire)) return false;
        const auto head = write.load(std::memory_order_relaxed);
        if (head - read.load(std::memory_order_acquire) == Capacity) return false;
        pending[head % Capacity] = std::move(callback);
        write.store(head + 1, std::memory_order_release);
        wake.fetch_add(1, std::memory_order_release);
        wake.notify_one();
        return true;
    }

    // Only shutdown joins the worker. Finish before destroying its Vulkan device.
    void DrainAndStop() {
        {
            std::scoped_lock lock{producer_mutex};
            stopping.store(true, std::memory_order_release);
        }
        wake.fetch_add(1, std::memory_order_release);
        wake.notify_one();
        if (worker.joinable()) worker.join();
    }

private:
    bool TryPop(std::function<void()>& callback) {
        const auto tail = read.load(std::memory_order_relaxed);
        if (tail == write.load(std::memory_order_acquire)) return false;
        callback = std::move(pending[tail % Capacity]);
        read.store(tail + 1, std::memory_order_release);
        return true;
    }

    void Run() {
        auto burst_start = std::chrono::steady_clock::now();
        size_t burst = 0;
        for (;;) {
            const auto observed = wake.load(std::memory_order_acquire);
            std::function<void()> callback;
            if (!TryPop(callback)) {
                // Stop is published after the last accepted producer finished.
                // Recheck the queue after observing stop so that a concurrent
                // final publication cannot be lost between empty and stopping.
                if (stopping.load(std::memory_order_acquire)) {
                    if (!TryPop(callback)) return;
                } else {
                    wake.wait(observed, std::memory_order_acquire);
                    burst = 0;
                    burst_start = std::chrono::steady_clock::now();
                    continue;
                }
            }
            callback(); // no producer mutex held, including callback destruction
            callback = {};
            if (!stopping.load(std::memory_order_acquire) &&
                (++burst >= BurstLimit ||
                 std::chrono::steady_clock::now() - burst_start >= BurstBudget)) {
                // A full queue also leaves completed objects in the device's
                // retirement deque. Sleeping after every eight callbacks capped
                // reclamation below the menu's allocation rate, retaining VRAM
                // indefinitely. Keep the low-priority worker runnable while a
                // burst remains queued; never move destruction to the producer.
                const auto remaining = write.load(std::memory_order_acquire) -
                                       read.load(std::memory_order_relaxed);
                if (remaining >= BurstLimit) {
                    std::this_thread::yield();
                } else {
                    std::this_thread::sleep_for(YieldInterval);
                }
                burst = 0;
                burst_start = std::chrono::steady_clock::now();
            }
        }
    }

    std::array<std::function<void()>, Capacity> pending;
    std::mutex producer_mutex;
    std::atomic<size_t> write{0};
    std::atomic<size_t> read{0};
    std::atomic<size_t> wake{0};
    std::atomic<bool> stopping{false};
    std::thread worker;
};

} // namespace Native::Rhi
