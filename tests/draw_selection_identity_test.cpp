#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/draw_selection_identity.h"

TEST(DrawSelectionIdentity, IndirectRequiresExactSgprsAndKind) {
    EXPECT_TRUE(Vulkan::CanUseDrawSelection(true, false, 65535, 42, true, 65535, 42, false));
    EXPECT_FALSE(Vulkan::CanUseDrawSelection(true, false, 65535, 42, true, 65534, 42, true));
    EXPECT_FALSE(Vulkan::CanUseDrawSelection(true, false, 65535, 42, true, 65535, 43, true));
    EXPECT_FALSE(Vulkan::CanUseDrawSelection(true, false, 0, 0, false, 0, 0, true));
    EXPECT_FALSE(Vulkan::CanUseDrawSelection(false, true, 0, 0, true, 0, 0, true));
}

TEST(DrawSelectionIdentity, DirectAndQuadEligibilityRemainDistinct) {
    EXPECT_TRUE(Vulkan::CanUseDrawSelection(false, false, 0, 0, false, 0, 0, true));
    EXPECT_TRUE(Vulkan::CanUseDrawSelection(false, true, 0, 0, false, 0, 0, false));
    EXPECT_FALSE(Vulkan::CanUseDrawSelection(false, false, 0, 0, false, 0, 0, false));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
