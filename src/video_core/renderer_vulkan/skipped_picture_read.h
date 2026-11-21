// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "common/types.h"

// FIX-SGT-048: a picture read in a frame whose draws into it were all skipped for want of their
// pipeline. Its reader gets whatever an earlier scene left there. At GT7's race start (Tokyo in
// the rain, every launch) the motion-blur pictures held the last frame of the pre-race intro: the
// race's draws into them are skipped until their pipelines exist (PERF-GTS-493), and those waited
// behind about a thousand other draws' (the listing of their stored shaders, a translation queue
// that takes 8 at a time and drops what waited 33 ms, then the build queue). The blur pass read
// the intro's car and its headlights for the whole start transition, and the bloom showed them as
// a white cloud over the track. A skipped draw into a picture other draws still write in the
// frame (an object in the scene) only leaves a hole. A picture no draw wrote holds another frame
// whole. So what the draws into such a picture wait for goes first in each of those queues.
// Nothing waits for it: the draws stay skipped until it is ready.
namespace Vulkan::SkippedPictureRead {

/// A shader reads a picture in `frame`. `skipped_frame` is the frame a draw into it was last
/// skipped for want of its pipeline, and `written_frame` the frame anything last wrote it. True
/// when that frame's draws into it were all skipped: the reader sees an older frame whole.
constexpr bool ReadsOnlySkipped(u64 skipped_frame, u64 written_frame, u64 frame) {
    return frame != 0 && skipped_frame == frame && written_frame != frame;
}

/// A build to move to the front of the queue: not started yet, and not moved before. True once
/// per build, so a picture read every frame queues its build once, not once a frame.
template <typename Build>
bool ClaimPromotion(Build& build) {
    return !build.started.load(std::memory_order_acquire) &&
           !build.promoted.exchange(true, std::memory_order_acq_rel);
}

} // namespace Vulkan::SkippedPictureRead
