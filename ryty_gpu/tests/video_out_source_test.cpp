#include <cstdio>
#include "video_core/texture_cache/image_info.h"

using VideoCore::ImageInfo;

static ImageInfo Surface(bool alternate) {
    ImageInfo info{};
    info.guest_address = 0x280400000;
    info.guest_size = alternate ? 0x870000 : 0x7f8000;
    info.type = AmdGpu::ImageType::Color2D;
    info.size = {1920, 1080, 1};
    info.pitch = 1920;
    info.resources = {1, 1};
    info.pixel_format = vk::Format::eA2B10G10R10UnormPack32;
    info.num_bits = 32;
    info.num_samples = 1;
    info.tile_mode = AmdGpu::TileMode::Display2DThin;
    info.array_mode = AmdGpu::ArrayMode::Array2DTiledThin1;
    info.props.is_tiled = true;
    info.alt_tile = alternate;
    return info;
}

int main() {
    const auto display = Surface(true);
    const auto rendered = Surface(false);
    int failed = 0;
    const auto check = [&](bool value, const char* name) {
        if (!value) {
            std::fprintf(stderr, "FAIL: %s\n", name);
            ++failed;
        }
    };
    check(rendered.IsVideoOutSource(display), "captured normal-to-Pro display layout");
    check(display.IsVideoOutSource(rendered), "alternate-to-normal display layout");
    check(display.IsVideoOutSource(display), "unchanged alternate display layout");
    check(rendered.IsVideoOutSource(rendered), "unchanged normal display layout");
    auto unrelated = rendered;
    unrelated.guest_address += 0x10000;
    check(!unrelated.IsVideoOutSource(display), "different allocation");
    unrelated = rendered;
    unrelated.size.width = 1200;
    check(!unrelated.IsVideoOutSource(display), "origin-aligned crop");
    unrelated = rendered;
    unrelated.size.height /= 2;
    check(!unrelated.IsVideoOutSource(display), "half-height crop");
    unrelated = rendered;
    unrelated.pitch = 2048;
    check(!unrelated.IsVideoOutSource(display), "different pitch");
    unrelated = rendered;
    unrelated.pixel_format = vk::Format::eR8G8B8A8Unorm;
    check(!unrelated.IsVideoOutSource(display), "same-size different format");
    unrelated = rendered;
    unrelated.num_samples = 2;
    check(!unrelated.IsVideoOutSource(display), "multisampled source");
    unrelated = rendered;
    unrelated.resources.levels = 2;
    check(!unrelated.IsVideoOutSource(display), "mip chain");
    unrelated = rendered;
    unrelated.resources.layers = 2;
    check(!unrelated.IsVideoOutSource(display), "array image");
    unrelated = rendered;
    unrelated.props.is_depth = true;
    check(!unrelated.IsVideoOutSource(display), "depth image");
    unrelated = rendered;
    unrelated.props.is_block = true;
    check(!unrelated.IsVideoOutSource(display), "compressed image");
    unrelated = rendered;
    unrelated.tile_mode = AmdGpu::TileMode::DisplayLinearAligned;
    unrelated.props.is_tiled = false;
    check(!unrelated.IsVideoOutSource(display), "linear-versus-tiled image");
    unrelated = rendered;
    unrelated.type = AmdGpu::ImageType::Color3D;
    check(!unrelated.IsVideoOutSource(display), "3D image");
    if (failed) return 1;
    std::puts("PASS: captured VideoOut layout pair, both directions, and unrelated-image guards");
}
