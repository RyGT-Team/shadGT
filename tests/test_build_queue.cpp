// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/build_queue.h"

using Vulkan::BuildQueue;
using namespace std::chrono_literals;

namespace {
struct Popped {
    bool success{};
    bool background{};
    bool heavy{};
    bool read_ahead{};
    BuildQueue::Job job;
};

Popped Pop(BuildQueue& queue, std::stop_token stop = {}) {
    Popped result;
    result.success = queue.Pop(result.job, result.background, stop, &result.heavy, &result.read_ahead);
    return result;
}

void Finish(BuildQueue& queue, const Popped& result) {
    if (result.background) {
        queue.FinishedBackground(result.heavy);
    } else {
        queue.FinishedForeground(result.heavy, result.read_ahead);
    }
}

// Always stop and join the worker, including when an expectation fails.
class WaitingPop {
public:
    explicit WaitingPop(BuildQueue& queue_) : queue{queue_}, future{promise.get_future()},
        worker{[this](std::stop_token stop) { promise.set_value(Pop(queue, stop)); }} {}
    ~WaitingPop() {
        worker.request_stop();
        queue.NotifyAll();
    }
    bool Ready(std::chrono::milliseconds timeout = 2s) {
        return future.wait_for(timeout) == std::future_status::ready;
    }
    Popped Get() { return future.get(); }
    void Stop() { worker.request_stop(); queue.NotifyAll(); }
private:
    BuildQueue& queue;
    std::promise<Popped> promise;
    std::future<Popped> future;
    std::jthread worker;
};
} // namespace

TEST(BuildQueue, PriorityAndOrdering) {
    BuildQueue queue{4, 2};
    std::vector<std::string> order;
    const auto push = [&](const char* name, BuildQueue::Kind kind) {
        queue.Push([&, name] { order.emplace_back(name); }, kind);
    };
    push("background", BuildQueue::Kind::Background);
    push("normal1", BuildQueue::Kind::Normal);
    push("soon1", BuildQueue::Kind::Soon);
    push("urgent1", BuildQueue::Kind::Urgent);
    push("normal2", BuildQueue::Kind::Normal);
    push("soon2", BuildQueue::Kind::Soon);
    push("urgent2", BuildQueue::Kind::Urgent);
    for (int i = 0; i < 7; ++i) {
        auto result = Pop(queue);
        ASSERT_TRUE(result.success);
        result.job();
        Finish(queue, result);
    }
    EXPECT_EQ(order, (std::vector<std::string>{"urgent2", "urgent1", "soon1", "soon2",
                                               "normal1", "normal2", "background"}));
}

TEST(BuildQueue, SoonCountsAsPendingAndForegroundUntilFinished) {
    BuildQueue queue{2, 1};
    EXPECT_TRUE(queue.ForegroundIdle());
    queue.Push([] {}, BuildQueue::Kind::Soon);
    EXPECT_EQ(queue.Pending(), 1u);
    EXPECT_FALSE(queue.ForegroundIdle());
    auto result = Pop(queue);
    ASSERT_TRUE(result.success);
    EXPECT_FALSE(result.background);
    EXPECT_EQ(queue.Pending(), 0u);
    EXPECT_FALSE(queue.ForegroundIdle());
    Finish(queue, result);
    EXPECT_TRUE(queue.ForegroundIdle());
}

TEST(BuildQueue, SoonWakesWorkerWhileBackgroundPaused) {
    BuildQueue queue{2, 1};
    queue.PauseBackground(BuildQueue::Clock::now() + 1h);
    queue.Push([] {}, BuildQueue::Kind::Background);
    WaitingPop waiting{queue};
    EXPECT_FALSE(waiting.Ready(50ms));
    queue.Push([] {}, BuildQueue::Kind::Soon);
    const bool ready = waiting.Ready();
    EXPECT_TRUE(ready);
    if (ready) {
        auto result = waiting.Get();
        EXPECT_TRUE(result.success);
        EXPECT_FALSE(result.background);
        if (result.success) Finish(queue, result);
    }
    EXPECT_EQ(queue.BackgroundQueued(), 1u);
}

TEST(BuildQueue, SoonWaitsForSharedTotalCapacity) {
    BuildQueue queue{1, 1};
    queue.Push([] {}, BuildQueue::Kind::Background);
    auto first = Pop(queue);
    ASSERT_TRUE(first.success);
    ASSERT_TRUE(first.background);
    queue.Push([] {}, BuildQueue::Kind::Soon);
    WaitingPop waiting{queue};
    EXPECT_FALSE(waiting.Ready(50ms));
    Finish(queue, first);
    const bool ready = waiting.Ready();
    EXPECT_TRUE(ready);
    if (ready) {
        auto result = waiting.Get();
        EXPECT_TRUE(result.success);
        EXPECT_FALSE(result.background);
        if (result.success) Finish(queue, result);
    }
}

TEST(BuildQueue, HeavyCapacityIsSharedAndReleasedAcrossKinds) {
    for (auto kind : {BuildQueue::Kind::Urgent, BuildQueue::Kind::Soon,
                      BuildQueue::Kind::Normal, BuildQueue::Kind::Background}) {
        BuildQueue queue{3, 2};
        queue.Push([] {}, kind, true);
        auto first = Pop(queue);
        ASSERT_TRUE(first.success);
        ASSERT_TRUE(first.heavy);
        queue.Push([] {}, BuildQueue::Kind::Soon, true);
        WaitingPop waiting{queue};
        EXPECT_FALSE(waiting.Ready(50ms));
        Finish(queue, first);
        const bool ready = waiting.Ready();
        EXPECT_TRUE(ready);
        if (ready) {
            auto result = waiting.Get();
            EXPECT_TRUE(result.success);
            EXPECT_TRUE(result.heavy);
            EXPECT_FALSE(result.background);
            if (result.success) Finish(queue, result);
        }
    }
}

TEST(BuildQueue, BlockedHeavySoonAllowsLighterWorkToRun) {
    BuildQueue queue{3, 2};
    queue.Push([] {}, BuildQueue::Kind::Urgent, true);
    auto heavy = Pop(queue);
    ASSERT_TRUE(heavy.success);
    ASSERT_TRUE(heavy.heavy);
    queue.Push([] {}, BuildQueue::Kind::Soon, true);
    queue.Push([] {}, BuildQueue::Kind::Normal);
    queue.Push([] {}, BuildQueue::Kind::Background);

    WaitingPop normal_wait{queue};
    const bool normal_ready = normal_wait.Ready();
    EXPECT_TRUE(normal_ready);
    if (!normal_ready) {
        Finish(queue, heavy);
        return;
    }
    auto normal = normal_wait.Get();
    ASSERT_TRUE(normal.success);
    EXPECT_FALSE(normal.background);
    EXPECT_FALSE(normal.heavy);
    WaitingPop background_wait{queue};
    const bool background_ready = background_wait.Ready();
    EXPECT_TRUE(background_ready);
    if (!background_ready) {
        Finish(queue, normal);
        Finish(queue, heavy);
        return;
    }
    auto background = background_wait.Get();
    ASSERT_TRUE(background.success);
    EXPECT_TRUE(background.background);
    EXPECT_FALSE(background.heavy);
    Finish(queue, normal);
    Finish(queue, background);

    WaitingPop waiting{queue};
    EXPECT_FALSE(waiting.Ready(50ms));
    Finish(queue, heavy);
    const bool ready = waiting.Ready();
    EXPECT_TRUE(ready);
    if (ready) {
        auto result = waiting.Get();
        EXPECT_TRUE(result.success);
        EXPECT_TRUE(result.heavy);
        EXPECT_FALSE(result.background);
        if (result.success) Finish(queue, result);
    }
}
TEST(BuildQueue, BoostUsesSharedCapacityForForegroundAndBackground) {
    BuildQueue queue{1, 1, 2};
    queue.SetBoost(true);
    queue.Push([] {}, BuildQueue::Kind::Background);
    queue.Push([] {}, BuildQueue::Kind::Background);
    auto first = Pop(queue);
    auto second = Pop(queue);
    ASSERT_TRUE(first.success && second.success);
    EXPECT_TRUE(first.background && second.background);
    queue.Push([] {}, BuildQueue::Kind::Soon);
    WaitingPop waiting{queue};
    EXPECT_FALSE(waiting.Ready(50ms));
    Finish(queue, first);
    const bool ready = waiting.Ready();
    EXPECT_TRUE(ready);
    if (ready) {
        auto result = waiting.Get();
        EXPECT_TRUE(result.success);
        EXPECT_FALSE(result.background);
        if (result.success) Finish(queue, result);
    }
    Finish(queue, second);
}

TEST(BuildQueue, StopAndNotifyWakeWaitingWorker) {
    BuildQueue queue{2, 1};
    WaitingPop waiting{queue};
    EXPECT_FALSE(waiting.Ready(50ms));
    waiting.Stop();
    const bool ready = waiting.Ready();
    EXPECT_TRUE(ready);
    if (ready) EXPECT_FALSE(waiting.Get().success);
}

TEST(BuildQueue, ReadAheadPacingLeavesScreenAndSoonRunnable) {
    BuildQueue queue{4, 2};
    queue.SetReadAheadLimit(1);
    queue.Push([] {}, BuildQueue::Kind::Normal);
    auto first = Pop(queue);
    ASSERT_TRUE(first.success);
    ASSERT_TRUE(first.read_ahead);
    queue.Push([] {}, BuildQueue::Kind::Normal);
    WaitingPop waiter(queue);
    EXPECT_FALSE(waiter.Ready(30ms));
    queue.Push([] {}, BuildQueue::Kind::Screen);
    ASSERT_TRUE(waiter.Ready());
    auto screen = waiter.Get();
    EXPECT_FALSE(screen.read_ahead);
    Finish(queue, screen);
    WaitingPop next(queue);
    EXPECT_FALSE(next.Ready(30ms));
    queue.SetReadAheadLimit(2);
    ASSERT_TRUE(next.Ready());
    auto second = next.Get();
    EXPECT_TRUE(second.read_ahead);
    Finish(queue, second);
    Finish(queue, first);
}

TEST(BuildQueue, ScreenFollowsUrgentAndPrecedesSoon) {
    BuildQueue queue{4, 2};
    std::vector<int> order;
    queue.Push([&] { order.push_back(2); }, BuildQueue::Kind::Soon);
    queue.Push([&] { order.push_back(1); }, BuildQueue::Kind::Screen);
    queue.Push([&] { order.push_back(0); }, BuildQueue::Kind::Urgent);
    for (int i = 0; i != 3; ++i) {
        auto next = Pop(queue);
        ASSERT_TRUE(next.success);
        next.job();
        Finish(queue, next);
    }
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2}));
}

TEST(BuildQueue, CatchUpExpandsAndShrinksAdmissionWithoutInterruptingJobs) {
    BuildQueue queue{1, 1, 4};
    queue.Push([] {}, BuildQueue::Kind::Soon);
    auto first = Pop(queue);
    ASSERT_TRUE(first.success);
    queue.Push([] {}, BuildQueue::Kind::Soon);
    WaitingPop waiter(queue);
    EXPECT_FALSE(waiter.Ready(30ms));
    queue.SetForegroundLimit(2);
    ASSERT_TRUE(waiter.Ready());
    auto second = waiter.Get();
    queue.SetForegroundLimit(1);
    queue.Push([] {}, BuildQueue::Kind::Soon);
    WaitingPop third(queue);
    EXPECT_FALSE(third.Ready(30ms));
    Finish(queue, first);
    EXPECT_FALSE(third.Ready(30ms));
    Finish(queue, second);
    ASSERT_TRUE(third.Ready());
    auto last = third.Get();
    Finish(queue, last);
}

TEST(BuildQueue, PartialOptionalWorkerGrowthKeepsExistingWorkers) {
    std::vector<int> workers{1, 2};
    unsigned calls = 0;
    EXPECT_FALSE(Vulkan::TryGrowCompileWorkers(workers, 5, [&] {
        if (++calls == 2) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        workers.push_back(3);
    }));
    EXPECT_EQ(workers, (std::vector<int>{1, 2, 3}));
    EXPECT_TRUE(Vulkan::TryGrowCompileWorkers(workers, 5, [&] { workers.push_back(4); }));
    EXPECT_EQ(workers.size(), 5u);
}

TEST(BuildQueue, StartupBoostIgnoresGameplayReadAheadPacing) {
    BuildQueue queue{2, 2, 2};
    queue.SetReadAheadLimit(1);
    queue.Push([] {}, BuildQueue::Kind::Normal);
    auto first = Pop(queue);
    queue.Push([] {}, BuildQueue::Kind::Normal);
    WaitingPop waiter(queue);
    EXPECT_FALSE(waiter.Ready(30ms));
    queue.SetBoost(true);
    ASSERT_TRUE(waiter.Ready());
    auto second = waiter.Get();
    Finish(queue, second);
    Finish(queue, first);
}
