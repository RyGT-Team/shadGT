// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/native_fence_batch.h"

TEST(NativeFenceBatch, OnlyOptionalDirectHeldFlushesShareTheInterval) {
    using Vulkan::SubmitNativeFence;
    EXPECT_TRUE(SubmitNativeFence(true, true, false, false, false, true, 5'000'000, 0, 2'000'000));
    EXPECT_FALSE(SubmitNativeFence(true, true, false, false, false, true, 6'000'000, 5'000'000, 2'000'000));
    EXPECT_TRUE(SubmitNativeFence(true, true, false, false, false, true, 7'000'000, 5'000'000, 2'000'000));
    EXPECT_TRUE(SubmitNativeFence(true, true, false, false, false, true, 4'000'000, 5'000'000, 2'000'000));
    EXPECT_TRUE(SubmitNativeFence(true, true, false, false, false, true, 6'000'000, 5'000'000, 0));
    EXPECT_TRUE(SubmitNativeFence(true, false, true, false, false, true, 6'000'000, 5'000'000, 2'000'000));
    EXPECT_TRUE(SubmitNativeFence(true, false, false, true, false, true, 6'000'000, 5'000'000, 2'000'000));
    EXPECT_TRUE(SubmitNativeFence(true, false, false, false, true, true, 6'000'000, 5'000'000, 2'000'000));
    EXPECT_TRUE(SubmitNativeFence(true, false, false, false, false, false, 6'000'000, 5'000'000, 2'000'000));
    EXPECT_FALSE(SubmitNativeFence(true, false, false, false, false, true, 10'000'000, 5'000'000, 2'000'000));
    // Without an idle callback, a VO-label waiter can be the next operation.
    // Its preceding mandatory flush must never be batched.
    EXPECT_TRUE(SubmitNativeFence(false, true, false, false, false, true, 6'000'000, 5'000'000, 2'000'000));
}

TEST(NativeFenceBatch, ParsesAnExplicitOffAndRejectsMalformedIntervals) {
    using Vulkan::NativeFenceBatchNs;
    EXPECT_EQ(NativeFenceBatchNs(""), 2'000'000);
    EXPECT_EQ(NativeFenceBatchNs("0"), 0);
    EXPECT_EQ(NativeFenceBatchNs(" 2.5 "), 2'500'000);
    for (const auto value : {"-1", "nan", "inf", "1x", "2000", "1e1000"})
        EXPECT_EQ(NativeFenceBatchNs(value), 2'000'000);
}
