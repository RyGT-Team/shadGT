// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "ryty_gpu/native/high_cpu_writeback.h"
#include "video_core/renderer_vulkan/oneshot_shader.h"

TEST(HighCpuWriteback, HonorsThresholdOverrideAndBudget) {
    using D = Native::HighCpuWriteback;
    constexpr std::uint64_t mb = 1ull << 20;
    constexpr std::uint64_t base = 0xf000000000ull;
    EXPECT_EQ(Native::ClassifyHighCpuWriteback(base, 32 * mb, base, 1024 * mb, 0, false), D::NotNeeded);
    EXPECT_EQ(Native::ClassifyHighCpuWriteback(base, 8 * mb, base, 1, 1, false, 16 * mb), D::Existing);
    EXPECT_EQ(Native::ClassifyHighCpuWriteback(base - 1, 32 * mb, base, 1024 * mb, 0, true), D::NotNeeded);
    EXPECT_EQ(Native::ClassifyHighCpuWriteback(base, 32 * mb, base, 1024 * mb, 0, true), D::Admit);
    EXPECT_EQ(Native::ClassifyHighCpuWriteback(base + 1, 32 * mb, base, 0, 0, true), D::NotNeeded);
    EXPECT_EQ(Native::ClassifyHighCpuWriteback(base, 32 * mb + 1, base, 0, 0, true), D::NotNeeded);
    EXPECT_EQ(Native::ClassifyHighCpuWriteback(base, 32 * mb, base, 0, 0, true), D::Admit);
    EXPECT_EQ(Native::ClassifyHighCpuWriteback(base, 256 * mb, base, 1024 * mb, 256 * mb, true), D::Admit);
    EXPECT_EQ(Native::ClassifyHighCpuWriteback(base, 32 * mb, base, 128 * mb, 64 * mb, true), D::InsufficientBudget);
    EXPECT_EQ(Native::ClassifyHighCpuWriteback(base, 257 * mb, base, 0, 0, true), D::OverLimit);
    EXPECT_EQ(Native::ClassifyHighCpuWriteback(~std::uint64_t{0} - 1, 4, base, 0, 0, true), D::OverLimit);
    EXPECT_EQ(Native::ClassifyHighCpuWriteback(base, 32 * mb, base + 1, 0, 0, true), D::NotNeeded);
}

TEST(HighCpuWriteback, OnlySourceOneShotShadersWait) {
    EXPECT_TRUE(Vulkan::IsGt7OneShotShader(0x9b9bf2a4));
    EXPECT_TRUE(Vulkan::IsGt7OneShotShader(0x40327433));
    EXPECT_FALSE(Vulkan::IsGt7OneShotShader(0x40327434));
}
