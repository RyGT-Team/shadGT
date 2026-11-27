// SPDX-License-Identifier: GPL-2.0-or-later
// FEAT-SGT-203: the game's 1920x1080 picture placed in outputs of other shapes (UiHires::UiRect,
// the proxy's shape tests).
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/ui_fit_policy.h"

using namespace Vulkan::UiFit;

namespace {
void ExpectRect(const Rect& rect, int x, int y, unsigned w, unsigned h) {
    EXPECT_EQ(rect.x, x);
    EXPECT_EQ(rect.y, y);
    EXPECT_EQ(rect.width, w);
    EXPECT_EQ(rect.height, h);
}
} // namespace

TEST(UiFitPolicy, TallerScreensGetBandsAtTopAndBottom) {
    ExpectRect(Centred(1920, 1200, 1920, 1080), 0, 60, 1920, 1080);
    ExpectRect(Centred(2560, 1600, 1920, 1080), 0, 80, 2560, 1440);
    ExpectRect(Centred(1280, 800, 1920, 1080), 0, 40, 1280, 720);
    ExpectRect(Centred(1680, 1050, 1920, 1080), 0, 52, 1680, 945);
    ExpectRect(Centred(1600, 1200, 1920, 1080), 0, 150, 1600, 900); // 4:3
}

TEST(UiFitPolicy, WiderAndSameShapeUnchanged) {
    // What UiRect computed before FEAT-SGT-203.
    ExpectRect(Centred(3440, 1440, 1920, 1080), 440, 0, 2560, 1440);
    ExpectRect(Centred(5120, 1440, 1920, 1080), 1280, 0, 2560, 1440);
    ExpectRect(Centred(2560, 1080, 1920, 1080), 320, 0, 1920, 1080);
    ExpectRect(Centred(3840, 2160, 1920, 1080), 0, 0, 3840, 2160);
    ExpectRect(Centred(1920, 1080, 1920, 1080), 0, 0, 1920, 1080);
    ExpectRect(Centred(1920, 1200, 0, 1080), 0, 0, 1920, 1200);
}

TEST(UiFitPolicy, Shapes) {
    EXPECT_TRUE(Taller(1920, 1200, 1920, 1080));
    EXPECT_TRUE(Taller(1280, 800, 1920, 1080));
    EXPECT_FALSE(Taller(3440, 1440, 1920, 1080));
    EXPECT_FALSE(Taller(2560, 1440, 1920, 1080));
    EXPECT_TRUE(Wider(3440, 1440, 1920, 1080));
    EXPECT_FALSE(Wider(1920, 1200, 1920, 1080));
    EXPECT_FALSE(Wider(3840, 2160, 1920, 1080));
    EXPECT_TRUE(Covers(1920, 1200, 1920, 1080));
    EXPECT_TRUE(Covers(2560, 1600, 1920, 1080));
    EXPECT_FALSE(Covers(1680, 1050, 1920, 1080)); // no proxy there
    EXPECT_FALSE(Covers(1280, 800, 1920, 1080));  // Steam Deck: no proxy either
}

