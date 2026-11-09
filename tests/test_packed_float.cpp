// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "video_core/texture_cache/packed_float.h"

TEST(PackedFloat, ChannelFieldsReorderAndRoundTrip) {
    constexpr std::uint32_t guest = (0x456u << 21) | (0x123u << 10) | 0x2abu;
    constexpr std::uint32_t vulkan = (0x2abu << 22) | (0x123u << 11) | 0x456u;
    EXPECT_EQ(VideoCore::PackedFloatToVulkan(guest), vulkan);
    EXPECT_EQ(VideoCore::PackedFloatFromVulkan(vulkan), guest);
    std::uint32_t value = 0x4265a31bu;
    for (int i = 0; i != 10000; ++i) {
        value = value * 1664525u + 1013904223u;
        EXPECT_EQ(VideoCore::PackedFloatFromVulkan(VideoCore::PackedFloatToVulkan(value)), value);
    }
}

TEST(PackedFloat, NativeTilingVariantsHaveDistinctKeys) {
    constexpr std::uint64_t base = (1ull << 49) | (17ull << 40) | (32ull << 8) | 1;
    EXPECT_NE(VideoCore::NativePackedTilingKey(base, false, true),
              VideoCore::NativePackedTilingKey(base, true, true));
    EXPECT_NE(VideoCore::NativePackedTilingKey(base, true, true),
              VideoCore::NativePackedTilingKey(base, true, false));
}

TEST(PackedFloat, TilingDispatchIncludesSmallAndTrailingTexels) {
    EXPECT_EQ(VideoCore::NativeTilingGroups(4, 4), 1u);
    EXPECT_EQ(VideoCore::NativeTilingGroups(64 * 4, 4), 1u);
    EXPECT_EQ(VideoCore::NativeTilingGroups(65 * 4, 4), 2u);
    EXPECT_EQ(VideoCore::NativeTilingGroups(0, 4), 0u);
    EXPECT_EQ(VideoCore::NativeTilingGroups(64, 0), 0u);
}
