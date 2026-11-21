// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/readback_pixels.h"

namespace {
using Vulkan::ConvertReadbackPixels;
using Vulkan::ReadbackOrder;
using Pixel = std::array<u8, 4>;

TEST(ReadbackPixels, EightBitPackingDoesNotDetermineTransfer) {
    // The same channel codes are ordinary SDR or PQ BT.2020 depending on the source frame.
    for (bool bgra : {false, true}) {
        const auto format = bgra ? vk::Format::eB8G8R8A8Unorm : vk::Format::eR8G8B8A8Unorm;
        const Pixel source = bgra ? Pixel{56, 80, 96, 17} : Pixel{96, 80, 56, 17};
        Pixel output{};
        ASSERT_TRUE(ConvertReadbackPixels(source, output, format, false, ReadbackOrder::Rgba));
        EXPECT_EQ(output, (Pixel{96, 80, 56, 255}));
        ASSERT_TRUE(ConvertReadbackPixels(source, output, format, true, ReadbackOrder::Rgba));
        EXPECT_EQ(output, (Pixel{189, 120, 56, 255}));

        Pixel encoder = source;
        ASSERT_TRUE(ConvertReadbackPixels(encoder, encoder, format, true, ReadbackOrder::Bgra));
        EXPECT_EQ(encoder, (Pixel{56, 120, 189, 255}));
    }
}

TEST(ReadbackPixels, PackedTenBitChannelsReachTheEncoderInBgraOrder) {
    constexpr u32 r = 385, g = 321, b = 225;
    for (bool red_low : {false, true}) {
        const auto format =
            red_low ? vk::Format::eA2B10G10R10UnormPack32 : vk::Format::eA2R10G10B10UnormPack32;
        const u32 packed = (2u << 30) | ((red_low ? b : r) << 20) | (g << 10) | (red_low ? r : b);
        const Pixel source{u8(packed), u8(packed >> 8), u8(packed >> 16), u8(packed >> 24)};
        Pixel encoder = source;
        ASSERT_TRUE(ConvertReadbackPixels(encoder, encoder, format, true, ReadbackOrder::Bgra));
        EXPECT_EQ(encoder, (Pixel{57, 120, 189, 255}));
        Pixel sdr{};
        ASSERT_TRUE(ConvertReadbackPixels(source, sdr, format, false, ReadbackOrder::Rgba));
        EXPECT_EQ(sdr, (Pixel{96, 80, 56, 255}));
    }
}

TEST(ReadbackPixels, RejectsUnsupportedFormatOrIncompletePixel) {
    Pixel bytes{};
    EXPECT_FALSE(
        ConvertReadbackPixels(bytes, bytes, vk::Format::eUndefined, false, ReadbackOrder::Rgba));
    const std::span<u8> three = std::span{bytes}.first(3);
    EXPECT_FALSE(ConvertReadbackPixels(three, three, vk::Format::eR8G8B8A8Unorm, false,
                                       ReadbackOrder::Rgba));
    EXPECT_FALSE(ConvertReadbackPixels(bytes, three, vk::Format::eR8G8B8A8Unorm, false,
                                       ReadbackOrder::Rgba));
}
} // namespace
