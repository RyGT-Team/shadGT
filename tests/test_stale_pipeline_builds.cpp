// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <unordered_map>
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/build_queue.h"
#include "video_core/renderer_vulkan/stale_pipeline_builds.h"

using namespace std::chrono_literals;
namespace {
struct Build {
    std::atomic<bool> started{}, urgent{};
    bool preloading{}, async{};
    std::chrono::steady_clock::time_point queued;
    std::promise<void> promise;
};
}

TEST(StalePipelineBuilds, ExpiresOnlyUnclaimedOptionalJobsAndCompletesFuture) {
    const auto now = std::chrono::steady_clock::now();
    std::unordered_map<int, std::shared_ptr<Build>> jobs;
    for (int i = 0; i < 7; ++i) {
        auto job = std::make_shared<Build>();
        job->queued = now - 31s;
        jobs.emplace(i, std::move(job));
    }
    jobs[1]->urgent = true;
    jobs[2]->async = true;
    jobs[3]->preloading = true;
    jobs[4]->queued = now - 1s;
    jobs[5]->started = true;
    auto canceled = jobs[6];
    auto future = canceled->promise.get_future();
    EXPECT_EQ(Vulkan::ExpireStaleAheadBuilds(jobs, 0, now), 1u);
    EXPECT_EQ(jobs.size(), 6u);
    EXPECT_EQ(future.wait_for(0s), std::future_status::ready);
    EXPECT_NO_THROW(future.get());
    EXPECT_TRUE(canceled->started);
}

TEST(StalePipelineBuilds, CancellationAndWorkerClaimAreExclusive) {
    for (int i = 0; i < 200; ++i) {
        auto job = std::make_shared<Build>();
        auto future = job->promise.get_future();
        bool canceled = false, claimed = false;
        std::thread worker([&] {
            claimed = !job->started.exchange(true);
            if (claimed) job->promise.set_value();
        });
        std::thread cleaner([&] { canceled = Vulkan::CancelUnstartedPipeline(*job); });
        worker.join();
        cleaner.join();
        EXPECT_NE(canceled, claimed);
        EXPECT_EQ(future.wait_for(0s), std::future_status::ready);
        EXPECT_NO_THROW(future.get());
    }
}

TEST(StalePipelineBuilds, CancelledQueuedClosureBecomesANoopAndCompletesWaiter) {
    const auto now = std::chrono::steady_clock::now();
    auto job = std::make_shared<Build>();
    job->queued = now - 31s;
    auto future = job->promise.get_future();
    std::unordered_map<int, std::shared_ptr<Build>> pending{{7, job}};
    Vulkan::BuildQueue queue{1, 1};
    unsigned compiled = 0;
    queue.Push([&] {
        if (!job->started.exchange(true)) { ++compiled; job->promise.set_value(); }
    }, Vulkan::BuildQueue::Kind::Normal);
    ASSERT_EQ(Vulkan::ExpireStaleAheadBuilds(pending, 8, now), 1u);
    EXPECT_EQ(future.wait_for(0s), std::future_status::ready);
    Vulkan::BuildQueue::Job closure;
    bool background = false, heavy = false, read_ahead = false;
    ASSERT_TRUE(queue.Pop(closure, background, {}, &heavy, &read_ahead));
    closure();
    queue.FinishedForeground(heavy, read_ahead);
    EXPECT_EQ(compiled, 0u);
    EXPECT_NO_THROW(future.get());
}
