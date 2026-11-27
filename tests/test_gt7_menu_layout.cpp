// SPDX-License-Identifier: GPL-2.0-or-later
// The 16:9 panel of GT7's menus inside a wider or a taller display (FEAT-SGT-243).
#include <utility>
#include <gtest/gtest.h>
#include "ryty_gpu/gt7_menu_layout.h"

using Gt7MenuLayout::Panel;
using Gt7MenuLayout::Shaped;

namespace {
void ExpectPanel(unsigned display_w, unsigned display_h, unsigned w, unsigned h) {
    const auto panel = Panel(display_w, display_h);
    EXPECT_EQ(panel.width, w) << display_w << "x" << display_h;
    EXPECT_EQ(panel.height, h) << display_w << "x" << display_h;
}
} // namespace

TEST(Gt7MenuLayout, WiderDisplaysKeepTheirHeight) {
    ExpectPanel(3440, 1440, 2560, 1440);
    ExpectPanel(2560, 1080, 1920, 1080);
    ExpectPanel(5120, 1440, 2560, 1440);
    ExpectPanel(3840, 1080, 1920, 1080);
    ExpectPanel(2576, 1080, 1920, 1080);
    EXPECT_TRUE(Shaped(3440, 1440));
}

TEST(Gt7MenuLayout, TallerDisplaysKeepTheirWidth) {
    ExpectPanel(1920, 1200, 1920, 1080); // 16:10
    ExpectPanel(1920, 1440, 1920, 1080); // 4:3
    ExpectPanel(2560, 1600, 2560, 1440);
    ExpectPanel(3456, 2160, 3456, 1944);
    EXPECT_TRUE(Shaped(1920, 1200));
    EXPECT_TRUE(Shaped(1920, 1440));
}

TEST(Gt7MenuLayout, SixteenByNineDisplaysAreTheirOwnPanel) {
    for (const auto [w, h] : {std::pair{1280u, 720u}, {1600u, 900u}, {1920u, 1080u},
                              {2560u, 1440u}, {3200u, 1800u}, {3840u, 2160u}}) {
        ExpectPanel(w, h, w, h);
        EXPECT_FALSE(Shaped(w, h)) << w << "x" << h;
    }
}
