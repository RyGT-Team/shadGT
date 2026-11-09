// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/texture_cache/tiling_specialization.h"
#include <cstdio>
#include <set>
#include <stdexcept>

void assert_fail_impl() { throw std::runtime_error("tiling assertion"); }
[[noreturn]] void unreachable_impl() {
  throw std::runtime_error("invalid tiling input");
}
namespace Common::Log {
void VLog(Class, Level, const char *, int, const char *,
          fmt::string_view format, fmt::format_args args) {
  throw std::runtime_error(fmt::vformat(format, args));
}
} // namespace Common::Log

int main() {
  VideoCore::ImageInfo image{};
  image.tile_mode = AmdGpu::TileMode::Display2DThin;
  image.array_mode = AmdGpu::ArrayMode::Array2DTiledThin1;
  image.num_bits = 32;
  image.num_samples = 1;
  int failures = 0;
  const auto check = [&](bool pass, const char *message) {
    if (!pass) {
      std::fprintf(stderr, "FAIL: %s\n", message);
      ++failures;
    }
  };
  const std::array<u32, 12> base_display{1, 0, 1,  0, 4,   12,
                                         1, 1, 16, 4, 512, 2};
  const std::array<u32, 12> pro_display{1, 0, 1, 0, 4, 16, 1, 2, 8, 3, 512, 1};
  check(VideoCore::TilingSpecialization(image, false) == base_display,
        "base display preserves its existing pipe/bank layout");
  image.alt_tile = true;
  check(VideoCore::TilingSpecialization(image, false) == pro_display,
        "Neo display selects 16 pipes and alternate bank geometry");
  auto pro_write = pro_display;
  pro_write[3] = 1;
  check(VideoCore::TilingSpecialization(image, true) == pro_write,
        "Neo readback uses the same layout as uploads");
  image.tile_mode = AmdGpu::TileMode::Thin1DThin;
  image.array_mode = AmdGpu::ArrayMode::Array1DTiledThin1;
  const std::array<u32, 12> micro{1, 1, 1, 0};
  check(VideoCore::TilingSpecialization(image, false) == micro,
        "micro tiling has no macro pipe/bank specialization");
  std::set<u64> keys;
  for (const auto mode :
       {AmdGpu::TileMode::Display2DThin, AmdGpu::TileMode::Thin2DThin}) {
    for (u32 bits : {8u, 16u, 32u, 64u, 128u}) {
      for (u32 samples : {1u, 2u, 4u}) {
        for (bool alternate : {false, true}) {
          for (bool write : {false, true}) {
            image.tile_mode = mode;
            image.num_bits = bits;
            image.num_samples = samples;
            image.alt_tile = alternate;
            check(
                keys.insert(VideoCore::TilingPipelineKey(image, write)).second,
                "base/Neo, read/write, format and MSAA variants never alias");
          }
        }
      }
    }
  }
  if (!failures) {
    std::puts("PASS: base/Neo display geometry, micro tiling and 120 distinct "
              "native pipeline variants");
  }
  return failures ? 1 : 0;
}
