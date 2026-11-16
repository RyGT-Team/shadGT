// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>

#include <gtest/gtest.h>

#include "ryty_gpu/native/prepared_lifetime.h"

namespace {

using Native::CommandRange;
using Native::InCommandRanges;
using Native::NormalizeCommandRanges;
using Native::RetiredCommandMemory;

// FIX-GTS-492: GT Sport's command chunks are 128 KB, handed out again and again.
constexpr u64 Chunk = 0x20000;
constexpr u64 A = 0x202b60000; // a chunk of the frame that ends
constexpr u64 B = A + 4 * Chunk; // a chunk of the next frame, alive across the interrupt

TEST(PreparedLifetime, RangesAreSortedAndMerged) {
    std::vector<CommandRange> ranges{
        {A + Chunk, A + 2 * Chunk}, {A, A + Chunk}, {B, B + 0x100}, {B + 0x80, B + 0x200}, {7, 7}};
    NormalizeCommandRanges(ranges);
    ASSERT_EQ(ranges.size(), 2u); // touching chunks and overlapping ones are one range
    EXPECT_EQ(ranges[0].begin, A);
    EXPECT_EQ(ranges[0].end, A + 2 * Chunk);
    EXPECT_EQ(ranges[1].begin, B);
    EXPECT_EQ(ranges[1].end, B + 0x200);
}

TEST(PreparedLifetime, AnAddressIsInItsRangeOnly) {
    std::vector<CommandRange> ranges{{A, A + Chunk}, {B, B + Chunk}};
    NormalizeCommandRanges(ranges);
    EXPECT_TRUE(InCommandRanges(ranges, A));
    EXPECT_TRUE(InCommandRanges(ranges, A + Chunk - 4));
    EXPECT_FALSE(InCommandRanges(ranges, A + Chunk)); // the end is not in it
    EXPECT_FALSE(InCommandRanges(ranges, A - 4));
    EXPECT_TRUE(InCommandRanges(ranges, B + 0x283c));
    EXPECT_FALSE(InCommandRanges({}, A));
}

// The lost device of 2026-11-16: a prepared draw of a frame long gone, still under its packet's
// address when another draw's packet was written there.
TEST(PreparedLifetime, AJobThatEndedBeforeTheInterruptCannotPublishIntoRetiredMemory) {
    RetiredCommandMemory retired;
    const u64 job_end = retired.Serial(); // the job ends: its chunk is its own
    std::vector<CommandRange> frame{{A, A + Chunk}};
    retired.Retire(frame); // its frame's end-of-pipe interrupt
    EXPECT_TRUE(retired.RetiredSince(job_end, A + 0x283c)); // a late walk of that job
}

TEST(PreparedLifetime, AJobOfTheNextFrameKeepsItsPreparedDraws) {
    RetiredCommandMemory retired;
    const u64 job_end = retired.Serial(); // built while the frame before is still on the GPU
    std::vector<CommandRange> frame{{A, A + Chunk}};
    retired.Retire(frame);
    EXPECT_FALSE(retired.RetiredSince(job_end, B + 0x283c)); // its chunk was not in that frame
}

TEST(PreparedLifetime, AJobThatBeginsInARecycledChunkIsNotRefused) {
    RetiredCommandMemory retired;
    std::vector<CommandRange> frame{{A, A + Chunk}};
    retired.Retire(frame);
    const u64 job_end = retired.Serial(); // a later job got chunk A from the pool and ended
    EXPECT_FALSE(retired.RetiredSince(job_end, A + 0x283c));
    std::vector<CommandRange> other{{B, B + Chunk}};
    retired.Retire(other); // an interrupt that does not cover A
    EXPECT_FALSE(retired.RetiredSince(job_end, A + 0x283c));
    std::vector<CommandRange> again{{A, A + Chunk}};
    retired.Retire(again); // the job's own frame ends
    EXPECT_TRUE(retired.RetiredSince(job_end, A + 0x283c));
}

TEST(PreparedLifetime, TooManyInterruptsAgoCountsAsRetired) {
    RetiredCommandMemory retired;
    const u64 job_end = retired.Serial();
    for (u64 i = 0; i < RetiredCommandMemory::Kept; ++i) {
        std::vector<CommandRange> other{{B + i * Chunk, B + (i + 1) * Chunk}};
        retired.Retire(other);
    }
    EXPECT_FALSE(retired.RetiredSince(job_end, A)); // all of them are still known
    std::vector<CommandRange> one_more{{B, B + Chunk}};
    retired.Retire(one_more);
    EXPECT_TRUE(retired.RetiredSince(job_end, A)); // the first one is forgotten: no proof left
}

TEST(PreparedLifetime, RetireGivesTheRangesBackNormalized) {
    RetiredCommandMemory retired;
    std::vector<CommandRange> frame{{B, B + Chunk}, {A, A + Chunk}, {A + Chunk, A + 2 * Chunk}};
    retired.Retire(frame);
    ASSERT_EQ(frame.size(), 2u);
    EXPECT_EQ(frame[0].begin, A);
    EXPECT_EQ(frame[0].end, A + 2 * Chunk);
    EXPECT_EQ(retired.Serial(), 1u);
}

// A job walked but never submitted: no retirement names its memory, its prepared draws go by
// age. The four stale ones of the lost device were 484 to 1,384 submissions old; a race's own
// are 2 or 4.
TEST(PreparedLifetime, OnlyAnOldPreparedDrawGoesByAge) {
    EXPECT_FALSE(Native::PreparedDrawTooOld(5770, 5772));
    EXPECT_FALSE(Native::PreparedDrawTooOld(5768, 5772));
    EXPECT_FALSE(Native::PreparedDrawTooOld(5772 - Native::MaxPreparedDrawAge, 5772));
    EXPECT_TRUE(Native::PreparedDrawTooOld(5772 - Native::MaxPreparedDrawAge - 1, 5772));
    EXPECT_TRUE(Native::PreparedDrawTooOld(4388, 5772));
    EXPECT_FALSE(Native::PreparedDrawTooOld(0xfffffffeu, 2)); // the count wrapped: 4 old
}

} // namespace
