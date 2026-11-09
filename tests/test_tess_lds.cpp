// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/tess_lds_scratch.h"

TEST(TessLdsScratch, BoundedAndOverflowSafe) {
    using Vulkan::TessLdsScratchSize;
    ASSERT_TRUE(TessLdsScratchSize(16, 2, 4, 64));
    EXPECT_EQ(*TessLdsScratchSize(16, 2, 4, 64), 2048);
    EXPECT_EQ(*TessLdsScratchSize(0, 0, 0, 0), 64);
    EXPECT_FALSE(TessLdsScratchSize(~0ull, ~0ull, 1, 64));
    EXPECT_FALSE(TessLdsScratchSize(1, 1, ~0ull, 64));
    EXPECT_FALSE(TessLdsScratchSize(1, 1, 1, ~0ull));
}

TEST(TessLdsScratch, PadsFinalGroupAndForwardReaderWithoutOverflow) {
    using Vulkan::TessLdsScratchLayout;
    auto group = TessLdsScratchLayout(3, 1, 1, 80, 2, 1);
    ASSERT_TRUE(group);
    EXPECT_EQ(group->written, 240);
    EXPECT_EQ(group->size, 320);
    auto forward = TessLdsScratchLayout(3, 1, 1, 80, 2, 2);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->written, 240);
    EXPECT_EQ(forward->size, 320);
    auto aligned = TessLdsScratchLayout(4, 1, 1, 80, 2, 1);
    ASSERT_TRUE(aligned);
    EXPECT_EQ(aligned->written, aligned->size);
    EXPECT_FALSE(TessLdsScratchLayout(~0ull, 1, 1, 80, 2, 2));
    EXPECT_FALSE(TessLdsScratchLayout(3, 2, 1, 80, 2, 1));
    EXPECT_FALSE(TessLdsScratchLayout(3, 1, 1, 528, 2, 1));
}

TEST(TessLdsScratch, AllocationContainsEveryPatchReaderAndWriter) {
    using Vulkan::TessLdsScratchLayout;
    for (const auto cp : {1ull, 3ull, 32ull}) {
        for (const auto patches : {1ull, 3ull, 17ull}) {
            for (const auto raw_group : {0ull, 1ull, 2ull, 16ull, 255ull}) {
                for (const auto mode : {1u, 2u}) {
                    const auto group = std::max(raw_group, 1ull);
                    const auto layout = TessLdsScratchLayout(patches * cp + cp - 1,
                        1, cp, 80, raw_group, mode);
                    ASSERT_TRUE(layout);
                    EXPECT_EQ(layout->written, patches * cp * 80);
                    EXPECT_LE(layout->written, layout->size);
                    for (std::uint64_t prim = 0; prim < patches; ++prim) {
                        const auto base = mode == 1 ? (prim / group) * group * cp : prim * cp;
                        const auto last_reader = base + group * cp - 1;
                        EXPECT_LT(last_reader * 80 + 79, layout->size);
                        EXPECT_LT(((prim + 1) * cp - 1) * 80 + 79, layout->written);
                    }
                }
            }
        }
    }
    EXPECT_FALSE(TessLdsScratchLayout(3, 1, ~0ull, 80, 2, 1));
    EXPECT_FALSE(TessLdsScratchLayout(3, 1, 1, 80, ~0ull, 2));
}

TEST(TessLdsScratch, RejectsUnsafeDraws) {
    using Vulkan::TessLdsSafeShape;
    EXPECT_TRUE(TessLdsSafeShape(1, false, true, false));
    EXPECT_FALSE(TessLdsSafeShape(0, false, true, false));
    EXPECT_FALSE(TessLdsSafeShape(2, false, true, false));
    EXPECT_FALSE(TessLdsSafeShape(1, true, true, false));
    EXPECT_FALSE(TessLdsSafeShape(1, false, false, false));
    EXPECT_FALSE(TessLdsSafeShape(1, false, true, true));
    EXPECT_FALSE(TessLdsSafeShape(1, false, true, false, false));
}

TEST(TessLdsScratch, RawMaskSelectsUnmodifiedShader) {
    using Vulkan::TessLdsModeForMask;
    EXPECT_EQ(TessLdsModeForMask(2, ~0u, ~0u), 2u);
    EXPECT_EQ(TessLdsModeForMask(2, ~0u, 0xffff0000u), 0u);
    EXPECT_EQ(TessLdsModeForMask(2, 0xffff0000u, ~0u), 0u);
    EXPECT_EQ(TessLdsModeForMask(2, 0xffff0000u, 0xffff0000u), 0u);
    EXPECT_EQ(TessLdsModeForMask(0, ~0u, ~0u), 0u);
    // A ghost override only changes pushed words; raw registers still select mode zero.
    EXPECT_EQ(TessLdsModeForMask(2, ~0u, 0xffff0000u), 0u);
}
