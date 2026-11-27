// SPDX-License-Identifier: GPL-2.0-or-later
// FEAT-SGT-209: where GT Sport's race HUD pieces go on a screen taller than 16:9 (boxes measured
// on its 1920x1080 HUD, fractions of the HUD).
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/hud_tall_layout.h"

using namespace Vulkan::HudTall;

TEST(HudTallLayout, TopPiecesGoUp) {
    EXPECT_EQ(Move(0.02f, 0.20f, 0.03f, 0.11f), -1); // position and laps
    EXPECT_EQ(Move(0.77f, 0.98f, 0.06f, 0.24f), -1); // track map
    EXPECT_EQ(Move(0.44f, 0.56f, 0.226f, 0.259f), -1); // lap timer
    EXPECT_EQ(Move(0.44f, 0.56f, 0.226f, 0.259f, 0.025f), -1); // with the geometry's slack
    EXPECT_EQ(Move(0.30f, 0.70f, 0.00f, 0.08f), -1); // rear-view mirror
    // FIX-SGT-240: the track map's whole box (to 290 lines of 1080), inside its region.
    EXPECT_EQ(Move(0.766f, 1.00f, 0.06f, 0.2685f, 0.025f), -1);
    EXPECT_LT(0.2685f, TopEnd(1));
}

TEST(HudTallLayout, BottomRowGoesDown) {
    EXPECT_EQ(Move(0.17f, 0.62f, 0.86f, 0.99f), 1); // gauges
    EXPECT_EQ(Move(0.01f, 0.12f, 0.94f, 0.98f), 1); // lap time
    EXPECT_EQ(Move(0.80f, 0.99f, 0.76f, 0.99f), 1); // course map
}

// FIX-SGT-240: the bumper and bonnet views' large dashboard goes down whole.
TEST(HudTallLayout, LargeDashboardGoesDownWhole) {
    EXPECT_EQ(Move(0.40f, 0.60f, 0.667f, 0.75f), 1); // the rev bar and the speed's panel
    EXPECT_EQ(Move(0.40f, 0.60f, 0.667f, 0.75f, 0.025f), 1);
    EXPECT_EQ(Move(0.47f, 0.51f, 0.652f, 0.660f), 1); // the shift lights
    EXPECT_EQ(Move(0.44f, 0.56f, 0.623f, 0.640f), 1); // AUTO DRIVE above them
    EXPECT_EQ(Move(0.28f, 0.40f, 0.73f, 0.99f), 1); // the speedometer's dial
    EXPECT_EQ(Move(0.60f, 0.73f, 0.73f, 0.99f), 1); // the rev counter's dial
    EXPECT_EQ(Move(0.44f, 0.56f, 0.39f, 0.610f), 0); // the start countdown's ring stays
    EXPECT_EQ(Move(0.44f, 0.56f, 0.39f, 0.610f, 0.025f), 0);
    // Beside it the row starts where it did: the standings and the lap times grow down to there.
    EXPECT_EQ(Move(0.02f, 0.15f, 0.667f, 0.75f), 0);
    EXPECT_EQ(Move(0.80f, 0.99f, 0.667f, 0.75f), 0);
}

TEST(HudTallLayout, MiddleStays) {
    EXPECT_EQ(Move(0.02f, 0.15f, 0.16f, 0.51f), 0); // standings
    EXPECT_EQ(Move(0.02f, 0.15f, 0.12f, 0.51f), 0); // the standings with their top line
    EXPECT_EQ(Move(0.80f, 0.99f, 0.275f, 0.305f), 0); // the lap times' first row
    EXPECT_EQ(Move(0.80f, 0.99f, 0.275f, 0.305f, 0.025f), 0);
    EXPECT_EQ(Move(0.80f, 0.99f, 0.275f, 0.40f), 0); // three laps of them
    EXPECT_EQ(Move(0.40f, 0.60f, 0.40f, 0.55f), 0); // a message in the middle
    EXPECT_EQ(Move(0.00f, 1.00f, 0.00f, 1.00f), 0); // the whole HUD
    EXPECT_EQ(Move(0.20f, 0.80f, 0.60f, 0.90f), 0); // across the bottom row's top
}

TEST(HudTallLayout, RegionsTileTheHud) {
    double area = 0.0;
    for (const auto& r : Regions) {
        EXPECT_LT(r.u0, r.u1);
        EXPECT_LT(r.v0, r.v1);
        area += double(r.u1 - r.u0) * double(r.v1 - r.v0);
        // A region moves the way an element filling it would.
        EXPECT_EQ(Move(r.u0 + 0.01f, r.u1 - 0.01f, r.v0 + 0.001f, r.v1 - 0.001f, 0.0f), r.move);
    }
    EXPECT_NEAR(area, 1.0, 1e-5);
    // The standings' column keeps its pieces from 0.13, the middle from 0.265, the right from
    // 0.272 (FIX-SGT-240: under the track map's box, above the lap times).
    EXPECT_FLOAT_EQ(Regions[0].v1, LeftTop);
    EXPECT_FLOAT_EQ(Regions[4].v1, OtherTop);
    EXPECT_FLOAT_EQ(Regions[8].v1, RightTop);
    // FIX-SGT-240: the bottom row starts higher in the middle column only.
    EXPECT_FLOAT_EQ(Regions[1].v1, Bottom);
    EXPECT_FLOAT_EQ(Regions[5].v1, MiddleBottom);
    EXPECT_FLOAT_EQ(Regions[9].v1, Bottom);
}
