// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/skipped_picture_read.h"

using Vulkan::SkippedPictureRead::ClaimPromotion;
using Vulkan::SkippedPictureRead::ReadsOnlySkipped;

namespace {
struct Build {
    std::atomic<bool> started{};
    std::atomic<bool> promoted{};
};
} // namespace

// GT7's race start: the motion-blur picture's draw is skipped in frame 2575, nothing writes it,
// the blur pass reads it: the intro's frame is read whole.
TEST(SkippedPictureRead, AllDrawsSkippedThisFrame) {
    EXPECT_TRUE(ReadsOnlySkipped(2575, 2511, 2575));
    EXPECT_TRUE(ReadsOnlySkipped(2575, 0, 2575)); // never written at all
}

// An object of the scene skipped while other draws wrote the scene: a hole, not another frame.
TEST(SkippedPictureRead, OtherDrawsWroteItThisFrame) {
    EXPECT_FALSE(ReadsOnlySkipped(2575, 2575, 2575));
}

// A skip in an earlier frame says nothing about this frame's read (drawn since, or not drawn
// this frame at all: a picture kept from frame to frame).
TEST(SkippedPictureRead, SkipInAnotherFrame) {
    EXPECT_FALSE(ReadsOnlySkipped(2574, 2511, 2575));
    EXPECT_FALSE(ReadsOnlySkipped(0, 0, 2575));
    EXPECT_FALSE(ReadsOnlySkipped(0, 0, 0)); // no frame yet
}

// A build is moved to the front once, and never once a worker has started it.
TEST(SkippedPictureRead, PromotionOnce) {
    Build build;
    EXPECT_TRUE(ClaimPromotion(build));
    EXPECT_FALSE(ClaimPromotion(build));
    Build started;
    started.started = true;
    EXPECT_FALSE(ClaimPromotion(started));
    EXPECT_FALSE(started.promoted.load());
}
