#include <gtest/gtest.h>
#include "ryty_gpu/native/whole_layer_clear.h"

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
