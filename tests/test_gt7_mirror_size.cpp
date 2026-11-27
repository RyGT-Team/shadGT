// SPDX-License-Identifier: GPL-2.0-or-later
// FIX-SGT-242: GT7's rear-view mirror keeps its 16:9 size and camera in a wider or taller scene.
#include <gtest/gtest.h>
#include "ryty_gpu/gt7_mirror_size.h"

using Gt7MirrorSize::Default;
using Gt7MirrorSize::ForScene;

namespace {
// What GT7 makes of a setting in a scene: the mirror's size in pixels, its camera's angle and aspect.
struct Shown {
    float width, height, angle, aspect;
};
Shown Show(const Gt7MirrorSize::Setting& s, unsigned scene_w, unsigned scene_h) {
    return {float(s.width) / 1920.0f * float(scene_w), float(s.height) / 1080.0f * float(scene_h),
            s.scale * s.angle, float(s.width) / (s.scale * float(s.height))};
}
void ExpectAsSixteenByNine(unsigned scene_w, unsigned scene_h, unsigned panel_w, unsigned panel_h) {
    const Shown wanted = Show(Default, panel_w, panel_h);
    const Shown shown = Show(ForScene(scene_w, scene_h), scene_w, scene_h);
    // Within the rounding of the two integers (a layout unit is up to two pixels there).
    EXPECT_NEAR(shown.width, wanted.width, 3.0f);
    EXPECT_NEAR(shown.height, wanted.height, 1.0f);
    EXPECT_NEAR(shown.angle, wanted.angle, 0.001f);
    EXPECT_NEAR(shown.aspect, wanted.aspect, 0.001f);
}
} // namespace

TEST(Gt7MirrorSize, SixteenByNineScenesKeepTheGameSetting) {
    for (const auto [w, h] : {std::pair{1920u, 1080u}, {2560u, 1440u}, {3840u, 2160u}, {0u, 0u}}) {
        const auto s = ForScene(w, h);
        EXPECT_EQ(s.width, 800);
        EXPECT_EQ(s.height, 128);
        EXPECT_EQ(s.scale, 1.0f);
        EXPECT_EQ(s.angle, 20.0f);
    }
}

TEST(Gt7MirrorSize, WiderScenesKeepThePixelsAndTheCamera) {
    EXPECT_EQ(ForScene(3840, 1080).width, 400);
    EXPECT_EQ(ForScene(3840, 1080).height, 128);
    ExpectAsSixteenByNine(3840, 1080, 1920, 1080); // 32:9
    ExpectAsSixteenByNine(2560, 1080, 1920, 1080); // 21:9
    ExpectAsSixteenByNine(3440, 1440, 2560, 1440);
    ExpectAsSixteenByNine(5120, 1440, 2560, 1440);
}

TEST(Gt7MirrorSize, TallerScenesKeepThePixelsAndTheCamera) {
    EXPECT_EQ(ForScene(1920, 1440).width, 800);
    EXPECT_EQ(ForScene(1920, 1440).height, 96);
    ExpectAsSixteenByNine(1920, 1200, 1920, 1080); // 16:10
    ExpectAsSixteenByNine(1920, 1440, 1920, 1080); // 4:3
    ExpectAsSixteenByNine(2560, 1600, 2560, 1440);
    ExpectAsSixteenByNine(1920, 1536, 1920, 1080); // 5:4
}
