// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <gtest/gtest.h>
#include "video_core/amdgpu/resource.h"

namespace {

AmdGpu::Sampler Sharp(u64 raw0, u64 raw1) {
    AmdGpu::Sampler sampler{};
    sampler.raw0 = raw0;
    sampler.raw1 = raw1;
    return sampler;
}

} // namespace

// FIX-GTS-503 (shadgt7's FIX-GT7-178). S#s GT7 really binds (menus and two races, 2026-11-13): none has a reserved bit.
TEST(SamplerSharp, GameSamplersHaveNoReservedBits) {
    static constexpr std::array<std::array<u64, 2>, 12> real{{
        {0x0000000000000000ULL, 0x0000000000000000ULL},
        {0x00fff00000000092ULL, 0x0000000008500000ULL}, // trilinear, clamped
        {0x00fff00000000080ULL, 0x0000000008500000ULL},
        {0xfffff00000000080ULL, 0x0000000008100000ULL}, // Spa's trees: performance hints set
        {0xfffff00000000680ULL, 0x0000000008f00000ULL},
        {0x00fff00000000492ULL, 0x0000000008f00000ULL}, // anisotropic
        {0x00fff00000003036ULL, 0x8000000002500000ULL}, // compare, white border
        {0x00fff00000000491ULL, 0x4000000008f00000ULL}, // black border
        {0x00fff00000000012ULL, 0x000000000a503f00ULL}, // level of detail bias -1
        {0x00fff00000008012ULL, 0x0000000001000000ULL}, // unnormalized coordinates
        {0x00fff00000006012ULL, 0x0000000001000000ULL},
        {0x00fff000000001b6ULL, 0x000000000a500000ULL},
    }};
    for (const auto& raw : real) {
        const AmdGpu::Sampler sampler = Sharp(raw[0], raw[1]);
        EXPECT_FALSE(sampler.HasReservedBits());
        EXPECT_TRUE(sampler.Valid());
    }
}

// What ps 0x2a265dff's unused sampler slot held: an address and a count, floats.
TEST(SamplerSharp, LeftoversWithReservedBitsAreTold) {
    EXPECT_TRUE(Sharp(0x003800109d653158ULL, 0x2006f3ac0000024aULL).HasReservedBits());
    EXPECT_TRUE(Sharp(0x0000000000000000ULL, 0x3f80000000000000ULL).HasReservedBits());
    EXPECT_TRUE(Sharp(0x001c00105106b630ULL, 0x2006f3ac000005daULL).HasReservedBits());
}

// Word 3's bits 12 to 28 and nothing else: not word 0's bit 31 nor word 3's bit 29, which mean
// something on the PS4 Pro's GPU generation, nor the border colour's index (0-11) and type (30-31).
TEST(SamplerSharp, ReservedBitsAreWord3Bits12To28) {
    for (u32 bit = 0; bit < 64; ++bit) {
        EXPECT_FALSE(Sharp(1ULL << bit, 0).HasReservedBits());
        EXPECT_EQ(Sharp(0, 1ULL << bit).HasReservedBits(), bit >= 44 && bit <= 60);
    }
}

// A leftover that happens to have no reserved bit set is not told (it is dropped once unused, FIX-GTS-503).
TEST(SamplerSharp, OtherLeftoversPass) {
    EXPECT_FALSE(Sharp(0x3f80000000000000ULL, 0x0000000000000000ULL).HasReservedBits());
    EXPECT_FALSE(Sharp(0x00000000000000ffULL, 0x0000000000000076ULL).HasReservedBits());
    EXPECT_FALSE(Sharp(0x00000000bf800000ULL, 0x0000000000000000ULL).HasReservedBits());
}
