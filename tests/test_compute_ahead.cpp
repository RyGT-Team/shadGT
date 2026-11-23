// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// FEAT-SGT-118: the splash's compute builds stop at the game's release. The gate is the one
// ComputePipeline::WarmFromWords waits at (TakeHeavyGate), the queue the one the pre-translation
// fills and PrecompileCompute runs (ComputeAheadQueue). A build is what would reach the driver.

#include <atomic>
#include <semaphore>
#include <stop_token>
#include <vector>
#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/compute_ahead.h"

namespace {

using Vulkan::ComputeAheadQueue;
using Vulkan::TakeHeavyGate;

std::vector<u32> Recipe(u32 seed) {
    return {0x43574752, 2, seed, seed + 1, 0, 0, 0, 0, 1, 0x07230203};
}

// The review's order: the gate is busy, the game is released after the first try, then the gate
// is freed. The build had waited for the gate while the game was held; it must not start in play.
TEST(ComputeAhead, AGateFreedAfterTheReleaseBuildsNothing) {
    std::counting_semaphore<64> gate{0};
    bool held = true;
    u32 waits = 0;
    u32 builds = 0;
    if (TakeHeavyGate(gate, std::stop_token{}, [&] { return held; }, [&] {
            if (++waits == 1) {
                held = false;
                gate.release();
            }
        })) {
        ++builds;
        gate.release();
    }
    EXPECT_EQ(builds, 0U);
    EXPECT_EQ(waits, 1U);
    EXPECT_TRUE(gate.try_acquire()) << "the gate was not given back";
}

TEST(ComputeAhead, AReleaseWhileTheGateStaysBusyEndsTheWait) {
    std::counting_semaphore<64> gate{0};
    bool held = true;
    u32 waits = 0;
    EXPECT_FALSE(TakeHeavyGate(gate, std::stop_token{}, [&] { return held; }, [&] {
        ++waits;
        held = false;
    }));
    EXPECT_EQ(waits, 1U);
}

TEST(ComputeAhead, AFreeGateIsTakenWhileTheGameIsHeld) {
    std::counting_semaphore<64> gate{1};
    u32 waits = 0;
    ASSERT_TRUE(TakeHeavyGate(gate, std::stop_token{}, [] { return true; }, [&] { ++waits; }));
    EXPECT_EQ(waits, 0U);
    EXPECT_FALSE(gate.try_acquire()) << "the caller holds the gate";
    gate.release();
}

TEST(ComputeAhead, AStopEndsTheWait) {
    std::counting_semaphore<64> gate{0};
    std::stop_source stop;
    EXPECT_FALSE(TakeHeavyGate(gate, stop.get_token(), [] { return true; }, [&] {
        stop.request_stop();
        gate.release();
    }));
    EXPECT_TRUE(gate.try_acquire());
}

// The review counted 16 hashes for 16 identical recipes; a copy is kept once.
TEST(ComputeAhead, CopiesAreKeptOnce) {
    ComputeAheadQueue queue;
    for (u32 i = 0; i < 16; ++i) {
        EXPECT_EQ(queue.Add(Recipe(7), [] { return true; }), i == 0);
    }
    EXPECT_TRUE(queue.Add(Recipe(8), [] { return true; }));
    EXPECT_EQ(queue.Size(), 2U);
}

TEST(ComputeAhead, NothingIsKeptOnceTheGamePlays) {
    ComputeAheadQueue queue;
    for (u32 i = 0; i < 16; ++i) {
        EXPECT_FALSE(queue.Add(Recipe(i), [] { return false; }));
    }
    EXPECT_EQ(queue.Size(), 0U);
}

TEST(ComputeAhead, ARunAfterTheReleaseBuildsNothing) {
    ComputeAheadQueue queue;
    for (u32 i = 0; i < 3; ++i) {
        queue.Add(Recipe(i), [] { return true; });
    }
    std::atomic<u64> done{0};
    u32 builds = 0;
    const auto counts = queue.Run([] { return false; },
                                  [&](const std::vector<u32>&) { return ++builds, true; }, done);
    EXPECT_EQ(builds, 0U);
    EXPECT_EQ(counts.left, 3U);
    EXPECT_EQ(done.load(), 3U) << "the bar counts what was left";
    EXPECT_EQ(queue.Size(), 0U);
}

TEST(ComputeAhead, AReleaseDuringTheRunLeavesTheRest) {
    ComputeAheadQueue queue;
    for (u32 i = 0; i < 4; ++i) {
        queue.Add(Recipe(i), [] { return true; });
    }
    std::atomic<u64> done{0};
    bool held = true;
    u32 builds = 0;
    const auto counts = queue.Run([&] { return held; },
                                  [&](const std::vector<u32>&) {
                                      ++builds;
                                      held = false; // the hold's limit, during this build
                                      return true;
                                  },
                                  done);
    EXPECT_EQ(builds, 1U);
    EXPECT_EQ(counts.built, 1U);
    EXPECT_EQ(counts.left, 3U);
    EXPECT_EQ(done.load(), 4U);
}

TEST(ComputeAhead, EachRecipeIsBuiltOnceWhileHeld) {
    ComputeAheadQueue queue;
    for (u32 i = 0; i < 5; ++i) {
        queue.Add(Recipe(i % 3), [] { return true; });
    }
    std::atomic<u64> done{0};
    std::vector<u32> seen;
    const auto counts = queue.Run([] { return true; },
                                  [&](const std::vector<u32>& words) {
                                      seen.push_back(words[2]);
                                      return words[2] != 2; // one the driver refuses
                                  },
                                  done);
    EXPECT_EQ(seen.size(), 3U);
    EXPECT_EQ(counts.built, 2U);
    EXPECT_EQ(counts.failed, 1U);
    EXPECT_EQ(counts.left, 0U);
}

} // namespace
