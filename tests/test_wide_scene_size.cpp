// SPDX-License-Identifier: GPL-2.0-or-later
// WIDE-001's scene size at the screen's shape, and FEAT-SGT-210's on screens taller than 16:9.
#include <gtest/gtest.h>
#include "ryty_gpu/wide_scene_size.h"

using Vulkan::WideScene::Extent;
using Vulkan::WideScene::MaxPixels;
using Vulkan::WideScene::Size;

namespace {
void ExpectSize(Extent e, unsigned w, unsigned h) {
    EXPECT_EQ(e.width, w);
    EXPECT_EQ(e.height, h);
}
constexpr Extent Base{1920, 1080};
} // namespace

TEST(WideSceneSize, TallerScreensKeepTheWidth) {
    ExpectSize(Size(Base, 1.6, 0, true), 1920, 1200);            // 16:10
    ExpectSize(Size(Base, 4.0 / 3.0, 0, true), 1920, 1440);      // 4:3
    ExpectSize(Size(Base, 1.6, 1600, true), 2560, 1600);         // 2560x1600, launcher's height
    ExpectSize(Size(Base, 1280.0 / 800.0, 0, true), 1920, 1200); // Steam Deck's shape
}

TEST(WideSceneSize, WiderScreensUnchanged) {
    // What RytySceneSize computed before FEAT-SGT-210.
    ExpectSize(Size(Base, 3440.0 / 1440.0, 1440, false), 3440, 1440);
    ExpectSize(Size(Base, 3440.0 / 1440.0, 0, false), 2576, 1080);
    ExpectSize(Size(Base, 32.0 / 9.0, 1080, false), 3840, 1080);
    ExpectSize(Size(Base, 32.0 / 9.0, 1440, false), 5120, 1440);
    ExpectSize(Size(Base, 2560.0 / 1080.0, 0, false), 2560, 1080);
    // A wanted height outside 360..2160 is ignored.
    ExpectSize(Size(Base, 32.0 / 9.0, 100, false), 3840, 1080);
}

TEST(WideSceneSize, PixelBudget) {
    for (const double aspect : {1.6, 4.0 / 3.0, 32.0 / 9.0, 3440.0 / 1440.0}) {
        for (const unsigned wanted : {0u, 1080u, 1440u, 1600u, 2160u}) {
            for (const bool tall : {false, true}) {
                const auto e = Size(Base, aspect, wanted, tall);
                EXPECT_LE(double(e.width) * e.height, MaxPixels * 1.01) << aspect << " " << wanted;
                EXPECT_LE(e.width, 7680u);
                EXPECT_EQ(e.width % 16, 0u);
                EXPECT_EQ(e.height % 8 == 0 || e.height == wanted || e.height == 1080, true);
            }
        }
    }
    // 4:3 at 2160 lines: 2880x2160, within 3840x2160's pixels.
    ExpectSize(Size(Base, 4.0 / 3.0, 2160, true), 2880, 2160);
}

TEST(WideSceneSize, OddInputsReturnTheBase) {
    ExpectSize(Size(Base, 0.0, 0, true), 1920, 1080);
    ExpectSize(Size(Extent{0, 0}, 1.6, 0, true), 0, 0);
}
