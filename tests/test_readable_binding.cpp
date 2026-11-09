#include <gtest/gtest.h>
#include "ryty_gpu/native/readable_binding.h"

TEST(ReadableBinding, CutsOnlyLargeReadOnlyUnimportedCpuRanges) {
    constexpr std::uint64_t size = 8ull << 20;
    unsigned region_calls = 0, import_calls = 0, read_calls = 0;
    bool gpu_region = false, imported = false;
    std::uint64_t readable = size / 2;
    const auto region = [&](auto, auto) { ++region_calls; return gpu_region; };
    const auto import = [&](auto, auto) { ++import_calls; return imported; };
    const auto prefix = [&](auto, auto) { ++read_calls; return readable; };
    const auto bind = [&](std::uint64_t n, bool written, bool enabled, bool direct) {
        return Native::ReadableBindingSize(0x1000, n, written, enabled, direct, region, import, prefix);
    };
    EXPECT_EQ(bind(size, false, true, true), size / 2);
    readable = 0;
    EXPECT_EQ(bind(size, false, true, true), 0u);
    gpu_region = true;
    EXPECT_EQ(bind(size, false, true, true), size);
    gpu_region = false; imported = true;
    EXPECT_EQ(bind(size, false, true, true), size);
    imported = false;
    EXPECT_EQ(bind(size, true, true, true), size);
    EXPECT_EQ(bind(size, false, false, true), size);
    EXPECT_EQ(bind(size, false, true, false), size);
    EXPECT_EQ(bind(1024, false, true, true), 1024u);
    EXPECT_EQ(read_calls, 2u);
    EXPECT_EQ(region_calls, 4u);
    EXPECT_EQ(import_calls, 3u);
}
