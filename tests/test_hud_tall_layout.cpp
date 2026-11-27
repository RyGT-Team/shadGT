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
}

TEST(HudTallLayout, BottomRowGoesDown) {
    EXPECT_EQ(Move(0.17f, 0.62f, 0.86f, 0.99f), 1); // gauges
    EXPECT_EQ(Move(0.01f, 0.12f, 0.94f, 0.98f), 1); // lap time
    EXPECT_EQ(Move(0.80f, 0.99f, 0.76f, 0.99f), 1); // course map
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
    // The standings' column keeps its pieces from 0.13, the others from 0.265 (lap times).
    EXPECT_FLOAT_EQ(Regions[0].v1, LeftTop);
    EXPECT_FLOAT_EQ(Regions[4].v1, OtherTop);
    EXPECT_FLOAT_EQ(Regions[8].v1, OtherTop);
}
