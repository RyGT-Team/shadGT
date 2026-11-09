#include <gtest/gtest.h>
#include "video_core/texture_cache/image_info.h"

TEST(ImagePackedLayout, OppositePackedOrderCannotAliasSameVulkanFormat) {
    VideoCore::ImageInfo larger{};
    larger.guest_address = 0x100000;
    larger.type = AmdGpu::ImageType::Color2D;
    larger.resources = {1, 1};
    larger.size = {128, 128, 1};
    larger.pixel_format = vk::Format::eB10G11R11UfloatPack32;
    larger.num_bits = 32; larger.num_samples = 1; larger.pitch = 128;
    larger.tile_mode = AmdGpu::TileMode::Thin1DThin;
    larger.mips_layout[0].pitch = 128;
    auto small = larger;
    small.size = {64, 64, 1};
    EXPECT_TRUE(small.IsSubrectOf(larger));
    small.props.is_bgr_packed_float = true;
    EXPECT_FALSE(small.IsSubrectOf(larger));
    auto same_size = larger;
    EXPECT_TRUE(same_size.IsVideoOutSource(larger));
    same_size.props.is_bgr_packed_float = true;
    EXPECT_FALSE(same_size.IsVideoOutSource(larger));
}
