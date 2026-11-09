#include <gtest/gtest.h>
#include "ryty_gpu/native/cube_view_policy.h"
TEST(CubeViewPolicy, RequiresWholeSupportedColorCubes) {
    using Native::CubeViewLayers;
    EXPECT_EQ(CubeViewLayers(true, false, 64, 64, 12, 1, 11), 6u);
    EXPECT_EQ(CubeViewLayers(true, false, 64, 64, 12, 1, 12), 12u);
    EXPECT_EQ(CubeViewLayers(true, false, 64, 64, 12, 1, 5), 0u);
    // Color and depth images follow the same Vulkan cube compatibility rule.
    EXPECT_EQ(CubeViewLayers(true, false, 64, 64, 6, 1, 6), 6u);
    EXPECT_EQ(CubeViewLayers(true, true, 64, 64, 12, 1, 6), 0u);
    EXPECT_EQ(CubeViewLayers(true, false, 64, 32, 12, 1, 6), 0u);
    EXPECT_EQ(CubeViewLayers(true, false, 64, 64, 12, 4, 6), 0u);
    EXPECT_EQ(CubeViewLayers(false, false, 64, 64, 12, 1, 6), 0u);
}
