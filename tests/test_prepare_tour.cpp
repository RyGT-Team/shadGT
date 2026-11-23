// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// FEAT-SGT-127: when a slice of the preparation tour is over, and whether it ran whole.

#include <gtest/gtest.h>
#include "ryty_gpu/prepare_tour.h"

namespace {

using PrepareTour::Slice;

TEST(PrepareTour, FirstLoadOpensTheSliceAndWaitsThirtySeconds) {
    Slice slice;
    EXPECT_TRUE(PrepareTour::NoteLoad(slice, 10.0));
    EXPECT_FALSE(PrepareTour::WentQuiet(slice, 39.9));
    EXPECT_TRUE(PrepareTour::WentQuiet(slice, 40.0));
}

TEST(PrepareTour, LaterLoadsDoNotReopen) {
    Slice slice;
    PrepareTour::NoteLoad(slice, 0.0);
    EXPECT_FALSE(PrepareTour::NoteLoad(slice, 1.0));
    EXPECT_EQ(slice.loads, 2u);
}

TEST(PrepareTour, CarsEndAfterTwentySeconds) {
    Slice slice;
    for (int i = 0; i < 100; ++i) {
        PrepareTour::NoteLoad(slice, i * 1.0 + (i == 50 ? 0.4 : 0.0)); // longest gap 1.4 s
    }
    EXPECT_DOUBLE_EQ(PrepareTour::QuietSeconds(slice), 20.0);
    EXPECT_FALSE(PrepareTour::WentQuiet(slice, 99.0 + 19.9));
    EXPECT_TRUE(PrepareTour::WentQuiet(slice, 99.0 + 20.0));
}

TEST(PrepareTour, CoursesWaitSixTimesTheirLongestGap) {
    Slice slice;
    PrepareTour::NoteLoad(slice, 0.0);
    PrepareTour::NoteLoad(slice, 5.5);
    PrepareTour::NoteLoad(slice, 13.8); // 8.3 s
    EXPECT_NEAR(PrepareTour::QuietSeconds(slice), 49.8, 1e-9);
}

TEST(PrepareTour, ASlowPcWaitsTwoMinutesAtMost) {
    Slice slice;
    PrepareTour::NoteLoad(slice, 0.0);
    PrepareTour::NoteLoad(slice, 40.0);
    EXPECT_DOUBLE_EQ(PrepareTour::QuietSeconds(slice), 120.0);
}

TEST(PrepareTour, AClosedSliceNeverWentQuiet) {
    Slice slice;
    EXPECT_FALSE(PrepareTour::WentQuiet(slice, 1000.0));
}

TEST(PrepareTour, ReopeningStartsAFreshSlice) {
    Slice slice;
    PrepareTour::NoteLoad(slice, 0.0);
    PrepareTour::NoteLoad(slice, 60.0);
    slice.open = false; // closed by its caller
    EXPECT_TRUE(PrepareTour::NoteLoad(slice, 100.0));
    EXPECT_EQ(slice.loads, 1u);
    EXPECT_DOUBLE_EQ(PrepareTour::QuietSeconds(slice), 30.0);
}

TEST(PrepareTour, WholeOnlyWhenEveryQueuedLoadStarted) {
    EXPECT_TRUE(PrepareTour::RanWhole(500, 500u));
    EXPECT_TRUE(PrepareTour::RanWhole(501, 500u));
    EXPECT_FALSE(PrepareTour::RanWhole(499, 500u));
    EXPECT_FALSE(PrepareTour::RanWhole(10, std::nullopt));
    EXPECT_FALSE(PrepareTour::RanWhole(0, 0u));
}

} // namespace
