// SPDX-License-Identifier: GPL-2.0-or-later
// FEAT-SGT-207: GT Sport's driving cameras keep their 16:9 width on screens taller than 16:9.
#include <cmath>
#include <limits>
#include <gtest/gtest.h>
#include "ryty_gpu/ryty_taller_view.h"

using Ryty::Camera::TallerViewFov;

namespace {
double HalfWidth(double fov_degrees, double aspect) {
    return std::tan(fov_degrees * 3.14159265358979323846 / 360.0) * aspect;
}
} // namespace

TEST(TallerView, KeepsTheSixteenNineWidth) {
    for (const float fov : {40.0f, 52.0f, 65.0f, 90.0f}) {
        for (const float aspect : {1.6f, 1.5f, 4.0f / 3.0f}) {
            const float taller = TallerViewFov(fov, aspect);
            EXPECT_GT(taller, fov);
            EXPECT_NEAR(HalfWidth(taller, aspect), HalfWidth(fov, 16.0 / 9.0), 1e-4);
        }
    }
    EXPECT_NEAR(TallerViewFov(52.0f, 1.6f), 56.9f, 0.05f); // the chase view on 16:10
}

TEST(TallerView, SixteenNineAndWiderUnchanged) {
    EXPECT_EQ(TallerViewFov(52.0f, 16.0f / 9.0f), 52.0f);
    EXPECT_EQ(TallerViewFov(52.0f, 1.77f), 52.0f);
    EXPECT_EQ(TallerViewFov(52.0f, 2.389f), 52.0f);
    EXPECT_EQ(TallerViewFov(52.0f, 0.0f), 52.0f);
}

TEST(TallerView, OddAnglesUnchanged) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(TallerViewFov(0.0f, 1.6f), 0.0f);
    EXPECT_EQ(TallerViewFov(-10.0f, 1.6f), -10.0f);
    EXPECT_EQ(TallerViewFov(179.5f, 1.6f), 179.5f);
    EXPECT_TRUE(std::isnan(TallerViewFov(nan, 1.6f)));
    EXPECT_EQ(TallerViewFov(178.0f, 0.3f), 178.0f); // would reach 179 degrees
}
