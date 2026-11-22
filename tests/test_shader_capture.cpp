// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// FEAT-SGT-117: where the preparation's capture finds a program's code from its footer.

#include <vector>
#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/shader_capture.h"

namespace {

using Vulkan::ShaderCapture::FindProgramStart;

constexpr u32 Token = 0xBEEB03FF;
constexpr u64 Low = 0x10'0000'0000;

// A block of guest words from Low, with a program at `start` whose footer is `bytes` further on.
std::vector<u32> Block(size_t words, u64 start, u64 bytes) {
    std::vector<u32> block(words, 0x7e000000); // v_nop-like filler, never the token
    const size_t i = size_t(start - Low) / 4;
    block[i] = Token;
    block[i + 1] = u32(bytes / 8 - 1);
    return block;
}

TEST(ShaderCapture, FindsTheStartThatPointsAtTheFooter) {
    const u64 start = Low + 0x300, footer = start + 0x520;
    const auto block = Block(0x1000, start, footer - start);
    EXPECT_EQ(FindProgramStart(block, Low, footer), start);
}

TEST(ShaderCapture, FindsAProgramAtTheBlockStart) {
    const u64 footer = Low + 0x88;
    const auto block = Block(0x100, Low, footer - Low);
    EXPECT_EQ(FindProgramStart(block, Low, footer), Low);
}

TEST(ShaderCapture, PassesOverATokenThatPointsElsewhere) {
    const u64 start = Low + 0x200, footer = start + 0x400;
    auto block = Block(0x1000, start, footer - start);
    // A nearer token whose operand names another footer: not this program.
    block[size_t(0x500) / 4] = Token;
    block[size_t(0x500) / 4 + 1] = 7;
    EXPECT_EQ(FindProgramStart(block, Low, footer), start);
}

TEST(ShaderCapture, NeedsTheStartAligned) {
    const u64 start = Low + 0x204, footer = start + 0x400; // not on a 256-byte boundary
    const auto block = Block(0x1000, start, footer - start);
    EXPECT_EQ(FindProgramStart(block, Low, footer), std::nullopt);
}

TEST(ShaderCapture, NothingWithoutAToken) {
    const std::vector<u32> block(0x1000, 0x7e000000);
    EXPECT_EQ(FindProgramStart(block, Low, Low + 0x800), std::nullopt);
}

TEST(ShaderCapture, RefusesAFooterBelowTheBlockOrAnUnalignedBlock) {
    const auto block = Block(0x1000, Low + 0x100, 0x100);
    EXPECT_EQ(FindProgramStart(block, Low + 0x100, Low), std::nullopt);
    EXPECT_EQ(FindProgramStart(block, Low + 4, Low + 0x200), std::nullopt);
}

TEST(ShaderCapture, DoesNotReadPastTheBlock) {
    // The footer's boundary is the block's last word: its operand word would be past the end.
    const u64 footer = Low + 0x3fc;
    std::vector<u32> block(0xc1, 0x7e000000); // words up to Low + 0x300, the boundary below the footer
    block.back() = Token;
    EXPECT_EQ(FindProgramStart(block, Low, footer), std::nullopt);
}

} // namespace
