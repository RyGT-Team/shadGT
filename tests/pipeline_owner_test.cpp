#include "video_core/renderer_vulkan/pipeline_owner.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <gtest/gtest.h>

namespace {
struct Pipeline { int value; };
}

TEST(PipelineOwner, CapturedSelectionSurvivesEraseRehashAndDeferredRecording) {
    std::unordered_map<int, std::shared_ptr<Pipeline>> cache;
    cache.emplace(1, std::make_shared<Pipeline>(42));
    const auto* selected = cache.at(1).get();
    std::shared_ptr<const void> owner = cache.at(1);
    std::weak_ptr<Pipeline> weak = cache.at(1);
    ASSERT_TRUE(Vulkan::PipelineOwnerMatches(selected, owner));
    std::function<int()> recording = [selected, owner] { return selected->value; };
    cache.erase(1);
    for (int i = 2; i < 1000; ++i) cache.emplace(i, std::make_shared<Pipeline>(i));
    owner.reset();
    EXPECT_FALSE(weak.expired());
    EXPECT_EQ(recording(), 42);
    recording = {};
    EXPECT_TRUE(weak.expired());
}

TEST(PipelineOwner, RejectsMismatchedAndMissingOwners) {
    auto first = std::make_shared<Pipeline>(1);
    auto second = std::make_shared<Pipeline>(2);
    std::shared_ptr<const void> owner = first;
    EXPECT_TRUE(Vulkan::PipelineOwnerMatches(first.get(), owner));
    EXPECT_FALSE(Vulkan::PipelineOwnerMatches(second.get(), owner));
    owner.reset();
    EXPECT_FALSE(Vulkan::PipelineOwnerMatches(first.get(), owner));
    EXPECT_FALSE(Vulkan::PipelineOwnerMatches(static_cast<Pipeline*>(nullptr), owner));
}

TEST(PipelineOwner, ListDeduplicatesAndRetiresAtEachSubmissionTick) {
    Vulkan::PipelineOwnerRetirement retirement;
    auto pipeline = std::make_shared<Pipeline>(42);
    std::weak_ptr<Pipeline> weak = pipeline;
    retirement.Hold(pipeline);
    retirement.Hold(pipeline);
    EXPECT_EQ(pipeline.use_count(), 2);
    retirement.Retire(3);
    // A later list using the same pipeline needs its own later retirement.
    retirement.Hold(pipeline);
    retirement.Retire(5);
    pipeline.reset();
    const auto release = [](std::vector<std::shared_ptr<const void>> owners) { owners.clear(); };
    retirement.Collect(3, release);
    EXPECT_FALSE(weak.expired());
    retirement.Collect(4, release);
    EXPECT_FALSE(weak.expired());
    retirement.Collect(5, release);
    EXPECT_TRUE(weak.expired());
}

TEST(PipelineOwner, CompletedBatchOwnsDistinctPipelinesUntilWorkerReleasesIt) {
    Vulkan::PipelineOwnerRetirement retirement;
    auto first = std::make_shared<Pipeline>(1);
    auto second = std::make_shared<Pipeline>(2);
    std::weak_ptr<Pipeline> weak_first = first, weak_second = second;
    retirement.Hold({});
    retirement.Hold(first);
    retirement.Hold(second);
    retirement.Hold(first);
    retirement.Retire(7);
    first.reset(); second.reset();
    std::vector<std::shared_ptr<const void>> queued;
    int calls = 0;
    retirement.Collect(6, [&](auto batch) { ++calls; queued = std::move(batch); });
    EXPECT_EQ(calls, 0);
    retirement.Collect(7, [&](auto batch) { ++calls; queued = std::move(batch); });
    ASSERT_EQ(calls, 1);
    ASSERT_EQ(queued.size(), 2);
    EXPECT_FALSE(weak_first.expired());
    EXPECT_FALSE(weak_second.expired());
    queued.clear();
    EXPECT_TRUE(weak_first.expired());
    EXPECT_TRUE(weak_second.expired());
}

TEST(PipelineOwner, GpuIdleClearsActiveAndPendingOwners) {
    Vulkan::PipelineOwnerRetirement retirement;
    auto first = std::make_shared<Pipeline>(1);
    auto second = std::make_shared<Pipeline>(2);
    std::weak_ptr<Pipeline> weak_first = first, weak_second = second;
    retirement.Hold(first);
    retirement.Retire(10);
    retirement.Hold(second);
    first.reset(); second.reset();
    retirement.ClearAfterGpuIdle();
    EXPECT_TRUE(weak_first.expired());
    EXPECT_TRUE(weak_second.expired());
}
