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

// The recorder's check at the take. Programs and registers as in a race draw: a vertex and a
// pixel shader, the vertex shader's register 2 pointing at the draw's own V# table.
struct Stage {
    u64 program;
    std::vector<u32> user_data;
};
std::vector<Native::StageIdentity> Identities(const std::vector<Stage>& stages) {
    std::vector<Native::StageIdentity> out;
    for (const Stage& stage : stages) {
        out.push_back({stage.program, stage.user_data});
    }
    return out;
}
bool Matches(const std::vector<Stage>& prepared, const std::vector<Stage>& draw) {
    return Native::PreparedStagesAreTheDraws(Identities(prepared), Identities(draw));
}
constexpr u64 Vs = 0x2001000, Ps = 0x2002000, Ls = 0x2003000, Hs = 0x2004000, Ds = 0x2005000;
// 16 registers as a job copies them; the draw's programs read the first 4 and 2.
std::vector<u32> Copied(u32 table, u32 leftover = 0) {
    std::vector<u32> registers(16, leftover);
    registers[0] = 0x11;
    registers[2] = table;
    return registers;
}

TEST(PreparedIdentity, TheDrawItWasMadeFor) {
    EXPECT_TRUE(Matches({{Ps, Copied(0x7000)}, {Vs, Copied(0x8000)}},
                        {{Ps, {0x11, 0}}, {Vs, {0x11, 0, 0x8000, 0}}}));
}

TEST(PreparedIdentity, RegistersTheProgramDoesNotReadMayDiffer) {
    // NAT-007: what earlier work left past the program's own registers is not the draw's.
    EXPECT_TRUE(Matches({{Vs, Copied(0x8000, 0xdead)}}, {{Vs, {0x11, 0xdead, 0x8000, 0xdead}}}));
    EXPECT_TRUE(Matches({{Vs, Copied(0x8000, 0xdead)}}, {{Vs, {0x11, 0xdead, 0x8000}}}));
}

// The common stale case: the same material's shaders, another frame's tables.
TEST(PreparedIdentity, TheSameShadersWithAnotherDrawsTablesIsRefused) {
    EXPECT_FALSE(Matches({{Ps, Copied(0x7000)}, {Vs, Copied(0x8000)}},
                         {{Ps, {0x11, 0}}, {Vs, {0x11, 0, 0x9000, 0}}}));
}

// The lost device: a tessellated prepared draw met by a plain draw. The LS and HS registers
// still name its programs, but their stages are off: they are not among the draw's programs.
TEST(PreparedIdentity, ATessellatedOneOnAPlainDrawIsRefused) {
    const std::vector<Stage> tessellated{
        {Ps, Copied(0x7000)}, {Hs, Copied(0x7100)}, {Ds, Copied(0x7200)}, {Ls, Copied(0x8000)}};
    EXPECT_FALSE(Matches(tessellated, {{Ps, {0x11, 0}}, {Vs, {0x11, 0, 0x8000, 0}}}));
    EXPECT_TRUE(Matches(tessellated, {{Ps, {0x11, 0}},
                                      {Ds, {0x11, 0, 0x7200}},
                                      {Hs, {0x11, 0, 0x7100}},
                                      {Ls, {0x11, 0, 0x8000}}}));
}

TEST(PreparedIdentity, AProgramReadingMoreRegistersThanWereCopiedIsRefused) {
    EXPECT_FALSE(Matches({{Vs, {0x11, 0}}}, {{Vs, {0x11, 0, 0x8000}}}));
}

} // namespace
