#include <gtest/gtest.h>
#include "ryty_gpu/native/whole_layer_clear.h"
#include "ryty_gpu/native/volume_fill.h"

TEST(VolumeFill, CopiesOneSliceAndHalfOfTheNext) {
    const auto plan = Native::PlanLinearVolumeFill(64, 64, 4, 64, 64, 4, 24 * 1024);
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->count, 2u);
    EXPECT_EQ(plan->regions[0], (Native::VolumeFillRegion{0, 0, 0, 64, 64, 1}));
    EXPECT_EQ(plan->regions[1], (Native::VolumeFillRegion{16 * 1024, 1, 0, 64, 32, 1}));
    EXPECT_EQ(plan->buffer_bytes, 24 * 1024u);
}

TEST(VolumeFill, PitchAndSlicePaddingKeepThePatternPhase) {
    const auto plan = Native::PlanLinearVolumeFill(5, 3, 4, 7, 5, 4, 176);
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->count, 3u);
    EXPECT_EQ(plan->regions[0], (Native::VolumeFillRegion{0, 0, 0, 5, 3, 1}));
    EXPECT_EQ(plan->regions[1], (Native::VolumeFillRegion{140, 1, 0, 5, 1, 1}));
    EXPECT_EQ(plan->regions[2], (Native::VolumeFillRegion{168, 1, 1, 2, 1, 1}));
    EXPECT_EQ(plan->buffer_bytes, 176u);
    const auto padding = Native::PlanLinearVolumeFill(5, 3, 4, 7, 5, 4, 91);
    ASSERT_TRUE(padding);
    ASSERT_EQ(padding->count, 1u);
    EXPECT_EQ(padding->buffer_bytes, 76u);
}

TEST(VolumeFill, RejectsPartialTexelsAndUnrepresentableSliceStrides) {
    EXPECT_FALSE(Native::PlanLinearVolumeFill(5, 3, 4, 7, 3, 4, 31));
    EXPECT_FALSE(Native::PlanLinearVolumeFill(5, 3, 4, 4, 3, 4, 12));
    EXPECT_FALSE(Native::PlanLinearVolumeFill(5, 3, 4, UINT32_MAX, UINT32_MAX, 4, UINT64_MAX));
}

TEST(WholeLayerClear, RequiresWholeEqualStrideSingleMip2D) {
    using Native::WholeLayerClearCount;
    EXPECT_EQ(WholeLayerClearCount(400, 200, 4, 4, 1, 1, 0, 400, true, false), 2u);
    EXPECT_EQ(WholeLayerClearCount(400, 100, 4, 4, 1, 1, 0, 400, true, false), 1u);
    EXPECT_EQ(WholeLayerClearCount(400, 201, 4, 4, 1, 1, 0, 400, true, false), 0u);
    EXPECT_EQ(WholeLayerClearCount(401, 200, 4, 4, 1, 1, 0, 401, true, false), 0u);
    EXPECT_EQ(WholeLayerClearCount(400, 400, 4, 4, 1, 1, 0, 400, true, false), 0u);
    EXPECT_EQ(WholeLayerClearCount(400, 200, 4, 3, 1, 1, 0, 400, true, false), 0u);
    EXPECT_EQ(WholeLayerClearCount(400, 200, 4, 4, 2, 1, 0, 400, true, false), 0u);
    EXPECT_EQ(WholeLayerClearCount(400, 200, 4, 4, 1, 2, 0, 400, true, false), 0u);
    EXPECT_EQ(WholeLayerClearCount(400, 200, 4, 4, 1, 1, 4, 400, true, false), 0u);
    EXPECT_EQ(WholeLayerClearCount(400, 200, 4, 4, 1, 1, 0, 399, true, false), 0u);
    EXPECT_EQ(WholeLayerClearCount(400, 200, 4, 4, 1, 1, 0, 400, false, false), 0u);
    EXPECT_EQ(WholeLayerClearCount(400, 200, 4, 4, 1, 1, 0, 400, true, true), 0u);
    EXPECT_EQ(WholeLayerClearCount(400, 200, 4, 4, 1, 1, 0, 400, true, false, false), 0u);
}

TEST(WholeLayerClear, RejectsAmbiguousPatternPhases) {
    EXPECT_TRUE(Native::WholeLayerPatternSafe(0x12345678, 0x12345678, 4, 1024));
    EXPECT_TRUE(Native::WholeLayerPatternSafe(0xabababab, 0xabababab, 1, 1023));
    EXPECT_FALSE(Native::WholeLayerPatternSafe(0x12345678, 0x12345678, 1, 1024));
    EXPECT_FALSE(Native::WholeLayerPatternSafe(0x12345678, 0x12345678, 4, 1023));
    EXPECT_FALSE(Native::WholeLayerPatternSafe(0x12345678, 0x87654321, 4, 1024));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
