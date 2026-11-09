#include <gtest/gtest.h>
#include <array>
#include <vector>
#include "ryty_gpu/native/layer_hash.h"

TEST(LayerHash, DetectsAnUnsampledChangeAgainstUploadedBytes) {
    constexpr std::uint64_t layer_size = 512 * 1024;
    constexpr std::uint32_t layers = 16;
    std::vector<std::uint8_t> data(layer_size * layers, 0);
    const auto hash = [&](std::uint32_t layer, std::uint64_t offset, std::uint64_t size) {
        std::uint64_t value = 1469598103934665603ull;
        for (std::uint64_t i = 0; i < size; ++i) value = (value ^ data[layer * layer_size + offset + i]) * 1099511628211ull;
        return value;
    };
    Native::LayerHashState state;
    state.Capture(layers, layer_size, hash);
    EXPECT_FALSE(state.Changed(layers, layer_size, 1, hash, true, true));
    data[12 * layer_size + 1000] = 1; // outside the four 64-byte samples
    EXPECT_TRUE(state.Changed(layers, layer_size, 2, hash, true, true)); // second half of layers
    EXPECT_FALSE(state.Changed(layers, layer_size, 2, hash, true, true)); // once per frame
    state.Capture(layers, layer_size, hash);
    EXPECT_FALSE(state.Changed(layers, layer_size, 3, hash, true, true));
}

TEST(LayerHash, FallsBackSafelyBeforeFirstUploadAndRotatesWholeLayerBudget) {
    Native::LayerHashState state;
    unsigned calls = 0;
    const auto hash = [&](std::uint32_t, std::uint64_t, std::uint64_t) { ++calls; return std::uint64_t{0}; };
    EXPECT_FALSE(state.Changed(16, 1024 * 1024, 1, hash, false, true));
    EXPECT_EQ(calls, 0u);
    state.Capture(16, 1024 * 1024, hash);
    calls = 0;
    EXPECT_FALSE(state.Changed(16, 1024 * 1024, 2, hash, false, true));
    EXPECT_EQ(calls, 4u);
}

TEST(LayerHash, TinyLayersNeverHashOutOfBoundsAndSamplesOnlyAvoidWholeHashes) {
    Native::LayerHashState state;
    unsigned calls = 0;
    const auto hash = [&](std::uint32_t, std::uint64_t, std::uint64_t) { ++calls; return std::uint64_t{0}; };
    state.Capture(0, 0, hash);
    state.Capture(100, 255, hash);
    EXPECT_EQ(calls, 0u);
    state.Capture(2, 256, hash, false);
    EXPECT_EQ(calls, 8u);
}
