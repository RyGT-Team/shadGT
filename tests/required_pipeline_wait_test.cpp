#include <future>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/required_pipeline_wait.h"
#include "video_core/renderer_vulkan/stale_pipeline_builds.h"

TEST(RequiredPipelineWait, ReplacementIsRejectedUntilCompletion) {
    std::mutex mutex;
    unsigned int waiters = 0;
    std::promise<void> entered, finish;
    auto done = finish.get_future().share();
    std::thread recorder([&] {
        std::unique_lock lock{mutex};
        Vulkan::WaitOutsideLookup(lock, waiters, [&] {
            entered.set_value();
            done.wait();
        });
        EXPECT_TRUE(lock.owns_lock());
        EXPECT_EQ(waiters, 0u);
    });
    entered.get_future().wait();
    {
        std::unique_lock lock{mutex};
        EXPECT_FALSE(Vulkan::CanReplaceShader(waiters, false));
        EXPECT_FALSE(Vulkan::CanCancelRequiredBuild(waiters));
        EXPECT_FALSE(Vulkan::CanReplaceShader(0, true));
    }
    finish.set_value();
    recorder.join();
    std::unique_lock lock{mutex};
    EXPECT_TRUE(Vulkan::CanReplaceShader(waiters, false));
    EXPECT_TRUE(Vulkan::CanCancelRequiredBuild(waiters));
}

TEST(RequiredPipelineWait, CancellationRelocksAndReleasesExclusion) {
    std::mutex mutex;
    unsigned int waiters = 0;
    std::unique_lock lock{mutex};
    std::promise<void> completion;
    auto cancelled = completion.get_future();
    completion.set_exception(std::make_exception_ptr(std::runtime_error("cancelled")));
    EXPECT_THROW(Vulkan::WaitOutsideLookup(lock, waiters, [&] {
        cancelled.get();
    }), std::runtime_error);
    EXPECT_TRUE(lock.owns_lock());
    EXPECT_EQ(waiters, 0u);
    EXPECT_TRUE(Vulkan::CanReplaceShader(waiters, false));
}

TEST(RequiredPipelineWait, RechecksStateAfterConcurrentInstallAndRehash) {
    std::mutex mutex;
    unsigned int waiters = 0;
    std::unordered_map<int, int> pipelines{{1, 0}};
    std::promise<void> waiting, finished;
    auto completion = finished.get_future().share();
    int selected = 0;
    std::thread recorder([&] {
        std::unique_lock lock{mutex};
        Vulkan::WaitOutsideLookup(lock, waiters, [&] {
            waiting.set_value();
            completion.wait();
        });
        // The draw must look the key up again: the old entry and its iterator
        // are no longer valid after the concurrent install and rehash.
        selected = pipelines.at(1);
    });
    waiting.get_future().wait();
    {
        std::unique_lock lock{mutex};
        pipelines.erase(1);
        for (int i = 2; i < 4096; ++i) pipelines.emplace(i, i);
        pipelines.emplace(1, 42);
    }
    finished.set_value();
    recorder.join();
    EXPECT_EQ(selected, 42);
}

TEST(RequiredPipelineWait, RejectsEveryTransientLookupMode) {
    EXPECT_TRUE(Vulkan::CanUnlockRequired(false, false, false, false));
    for (unsigned int mask = 1; mask < 16; ++mask) {
        EXPECT_FALSE(Vulkan::CanUnlockRequired(mask & 1, mask & 2, mask & 4, mask & 8));
    }
}

TEST(RequiredPipelineWait, RequiredBuildCannotBeCancelledWhileWaiting) {
    struct Build {
        std::atomic<bool> started{};
        std::promise<void> promise;
    } build;
    auto done = build.promise.get_future();
    unsigned int waiters = 1;
    const auto cancel = [&] {
        return Vulkan::CanCancelRequiredBuild(waiters) && Vulkan::CancelUnstartedPipeline(build);
    };
    EXPECT_FALSE(cancel());
    EXPECT_EQ(done.wait_for(std::chrono::milliseconds{0}), std::future_status::timeout);
    waiters = 0;
    EXPECT_TRUE(cancel());
    EXPECT_EQ(done.wait_for(std::chrono::milliseconds{0}), std::future_status::ready);
}

TEST(RequiredPipelineWait, ListingRemainsOwnedAfterConcurrentConsumptionAndRehash) {
    std::mutex mutex;
    unsigned int waiters = 0;
    std::promise<std::vector<int>> completion;
    std::unordered_map<int, std::shared_future<std::vector<int>>> listings;
    listings.emplace(1, completion.get_future().share());
    std::promise<void> waiting;
    std::vector<int> observed;
    std::thread reader([&] {
        std::unique_lock lock{mutex};
        const auto owned = listings.at(1);
        Vulkan::WaitOutsideLookup(lock, waiters, [&] {
            waiting.set_value();
            owned.wait();
        });
        observed = owned.get();
        EXPECT_TRUE(lock.owns_lock());
        EXPECT_EQ(waiters, 0u);
    });
    waiting.get_future().wait();
    {
        std::unique_lock lock{mutex};
        completion.set_value({11, 22, 33});
        const auto consumed = listings.at(1).get();
        EXPECT_EQ(consumed.size(), 3u);
        listings.erase(1);
        for (int i = 2; i < 4096; ++i) listings.emplace(i, std::shared_future<std::vector<int>>{});
    }
    reader.join();
    EXPECT_EQ(observed, (std::vector<int>{11, 22, 33}));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
