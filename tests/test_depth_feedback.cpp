// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "ryty_gpu/native/attachment_feedback.h"
#include "ryty_gpu/native/depth_snapshot_copies.h"
#include "ryty_gpu/native/writeback_plan.h"

TEST(DepthFeedback, ReadOnlyNeedsNoFeedbackExtension) {
    const auto result = Native::DecideDepthFeedback(true, false, false);
    EXPECT_TRUE(result.read_only);
    EXPECT_FALSE(result.active);
    EXPECT_FALSE(result.unsupported);
}

TEST(DepthFeedback, WritableRequiresBothFeatures) {
    EXPECT_TRUE(Native::DecideDepthFeedback(true, true, true).active);
    EXPECT_TRUE(Native::DecideDepthFeedback(true, true, false).unsupported);
    EXPECT_FALSE(Native::DecideDepthFeedback(false, true, false).unsupported);
}

TEST(DepthFeedback, TransitionAndSnapshotBothBlockAheadReuse) {
    EXPECT_FALSE(Native::BlocksParallelFeedback(false, false));
    EXPECT_TRUE(Native::BlocksParallelFeedback(true, false));
    EXPECT_TRUE(Native::BlocksParallelFeedback(false, true));
}

TEST(DepthFeedback, SnapshotCoversDepthStencilMipsAndLayers) {
    const auto copies = Native::DepthSnapshotCopies(16, 8, 3, 2, true);
    ASSERT_EQ(copies.size(), 6u);
    for (unsigned mip = 0; mip < 3; ++mip) {
        EXPECT_EQ(copies[2 * mip].srcSubresource.aspectMask, vk::ImageAspectFlagBits::eDepth);
        EXPECT_EQ(copies[2 * mip + 1].srcSubresource.aspectMask, vk::ImageAspectFlagBits::eStencil);
        EXPECT_EQ(copies[2 * mip].srcSubresource.layerCount, 2u);
        EXPECT_EQ(copies[2 * mip].extent.width, 16u >> mip);
        EXPECT_EQ(copies[2 * mip].extent.height, 8u >> mip);
    }
}

TEST(DepthFeedback, SnapshotSurvivesCopySpliceUntilDrawTick) {
    Native::RetiredWritebackPlans retired;
    auto owner = std::make_shared<int>(42);
    std::weak_ptr<int> weak = owner;
    std::vector<std::shared_ptr<void>> stream{owner};
    ASSERT_TRUE(retired.Retire(stream, 1)); // snapshot copy stream
    stream.push_back(owner);              // later draw stream
    owner.reset();
    ASSERT_TRUE(retired.Retire(stream, 2));
    retired.Collect(1);
    EXPECT_FALSE(weak.expired());
    retired.Collect(2);
    EXPECT_TRUE(weak.expired());
}
