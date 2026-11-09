// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include "ryty_gpu/ryty_pro_mode.h"
#include "core/libraries/videoout/fps_lock.h"

TEST(FastFlips, ExplicitEnvironmentAndConfigOverrideNativeDefaults) {
    using Libraries::VideoOut::FastFlipsEnabled;
    EXPECT_TRUE(FastFlipsEnabled(nullptr, std::nullopt, true, false));
    EXPECT_FALSE(FastFlipsEnabled(nullptr, std::nullopt, true, true));
    EXPECT_FALSE(FastFlipsEnabled(nullptr, std::nullopt, false, false));
    EXPECT_FALSE(FastFlipsEnabled(nullptr, false, true, false));
    EXPECT_TRUE(FastFlipsEnabled(nullptr, true, false, true));
    EXPECT_FALSE(FastFlipsEnabled("0", true, true, false));
    EXPECT_TRUE(FastFlipsEnabled("1", false, false, true));
    EXPECT_FALSE(Ryty::ParseConfigBool("{\"fast_flips\": false}", "fast_flips").value());
    EXPECT_TRUE(Ryty::ParseConfigBool("{\"fast_flips\": true}", "fast_flips").value());
    EXPECT_FALSE(Ryty::ParseConfigBool("{}", "fast_flips").has_value());
}

TEST(FastFlips, IntervalHonoursGuestRateAndNeverGoesNegative) {
    using namespace std::chrono_literals;
    using Libraries::VideoOut::FastFlipInterval;
    EXPECT_EQ(FastFlipInterval(16666666ns, 0), 15666666ns);
    EXPECT_EQ(FastFlipInterval(16666666ns, 1), 32333332ns);
    EXPECT_EQ(FastFlipInterval(500us, 0), 0ns);
}

namespace {

using Libraries::VideoOut::LockedFlipRate;

TEST(FpsLock, OffKeepsTheGameRate) {
    EXPECT_EQ(LockedFlipRate(0, 0, 60), 0);
    EXPECT_EQ(LockedFlipRate(1, 0, 60), 1);
}

TEST(FpsLock, ThirtyOnSixtyHertzFlipsEveryOtherVblank) {
    EXPECT_EQ(LockedFlipRate(0, 30, 60), 1);
}

TEST(FpsLock, NeverFasterThanTheGameAsks) {
    EXPECT_EQ(LockedFlipRate(2, 30, 60), 2); // the game's own 20 FPS
}

TEST(FpsLock, OtherRates) {
    EXPECT_EQ(LockedFlipRate(0, 20, 60), 2);
    EXPECT_EQ(LockedFlipRate(0, 60, 60), 0);  // no slower than the vblank: no lock
    EXPECT_EQ(LockedFlipRate(0, 120, 60), 0);
    EXPECT_EQ(LockedFlipRate(0, 45, 60), 1);  // rounds to whole vblanks, never faster
    EXPECT_EQ(LockedFlipRate(0, 30, 0), 0);   // no vblank rate known
}

} // namespace
