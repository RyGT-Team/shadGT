// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "video_core/renderer_vulkan/vk_common.h"

// NEO-GTS-508: the sample positions the PS4 Pro mode's multi-sample draws set
// (PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0..X1Y1), given to Vulkan with VK_EXT_sample_locations.
namespace Vulkan::SampleLocations {

/// The positions of one draw. samples 0: the pipeline has no sample-locations state.
/// A position is one byte: x in the low nibble, y in the high nibble, in 1/16 pixel from the
/// pixel's top-left corner (Vulkan's 0..0.9375). grid 1: one pattern for every pixel; grid 2:
/// one for each pixel of the 2x2 quad, in Vulkan's order ((x + y * 2) * samples + sample).
struct Pattern {
    std::uint8_t samples = 0;
    std::uint8_t grid = 1;
    std::array<std::uint8_t, 64> positions{};

    std::uint32_t Count() const {
        return std::uint32_t{samples} * grid * grid;
    }
    bool operator==(const Pattern&) const = default;
};

/// Vulkan's standard sample locations (the spec's table) for 2, 4, 8 or 16 samples, in 1/16.
inline Pattern Standard(std::uint32_t samples) {
    static constexpr std::array<std::uint8_t, 2 * 2> two{12, 12, 4, 4};
    static constexpr std::array<std::uint8_t, 4 * 2> four{6, 2, 14, 6, 2, 10, 10, 14};
    static constexpr std::array<std::uint8_t, 8 * 2> eight{9, 5, 7, 11, 13, 9, 5, 3,
                                                           3, 13, 1, 7, 11, 15, 15, 1};
    static constexpr std::array<std::uint8_t, 16 * 2> sixteen{
        9, 9, 7, 5, 5, 10, 12, 7, 3, 6, 10, 13, 13, 11, 11, 3,
        6, 14, 8, 1, 4, 2, 2, 12, 0, 8, 15, 4, 14, 15, 1, 0};
    const std::uint8_t* xy = samples == 2   ? two.data()
                             : samples == 4 ? four.data()
                             : samples == 8 ? eight.data()
                                            : sixteen.data();
    Pattern pattern;
    if (samples != 2 && samples != 4 && samples != 8 && samples != 16) {
        return pattern;
    }
    pattern.samples = static_cast<std::uint8_t>(samples);
    for (std::uint32_t i = 0; i < samples; ++i) {
        pattern.positions[i] = static_cast<std::uint8_t>(xy[i * 2] | (xy[i * 2 + 1] << 4));
    }
    return pattern;
}

/// NEO-GTS-511: why FromRegisters gave the standard positions (Game: it gave the game's).
enum class Why : std::uint8_t { Game, CountDiffers, OnePoint, QuadDiffers };

/// From the 16 registers PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0..X1Y1 (4 a pixel of the quad, in the
/// order X0Y0, X1Y0, X0Y1, X1Y1; a byte a sample, 4 a register: a signed 4-bit x then y in 1/16
/// pixel from the pixel's centre). register_samples: PA_SC_AA_CONFIG's count; samples: the
/// pipeline's; quad_grid: the device takes a 2x2 grid for this count. Vulkan's standard positions
/// when the counts differ, when the quad's pixels differ and quad_grid is false, or when all the
/// samples of a pixel sit on one point (registers never written); `why` says which.
inline Pattern FromRegisters(const std::array<std::uint32_t, 16>& regs,
                             std::uint32_t register_samples, std::uint32_t samples,
                             bool quad_grid, Why* why = nullptr) {
    const auto standard = [&](Why reason) {
        if (why) {
            *why = reason;
        }
        return Standard(samples);
    };
    if (samples < 2 || samples > 16 || register_samples != samples) {
        return standard(Why::CountDiffers);
    }
    // Signed nibbles -8..7 from the centre become 0..15 from the corner: add 8 to each.
    const auto position = [&](std::uint32_t pixel, std::uint32_t sample) {
        const std::uint32_t word = regs[pixel * 4 + sample / 4];
        const std::uint32_t byte = (word >> ((sample % 4) * 8)) & 0xff;
        return static_cast<std::uint8_t>((((byte & 0xf) + 8) & 0xf) |
                                         ((((byte >> 4) + 8) & 0xf) << 4));
    };
    bool alike = true;
    for (std::uint32_t pixel = 0; pixel < 4; ++pixel) {
        bool one_point = true;
        for (std::uint32_t sample = 0; sample < samples; ++sample) {
            alike = alike && position(pixel, sample) == position(0, sample);
            one_point = one_point && position(pixel, sample) == position(pixel, 0);
        }
        if (one_point) {
            return standard(Why::OnePoint);
        }
    }
    // NEO-GTS-511: 2-sample draws take the game's positions again (9d6b3efa kept them standard):
    // which 2-sample pipelines take custom positions is decided by Instance::TakesSampleLocations
    // and by the translation (no pipeline left at the standard positions on the same depth).
    if (!alike && !quad_grid) {
        return standard(Why::QuadDiffers);
    }
    if (why) {
        *why = Why::Game;
    }
    Pattern pattern;
    pattern.samples = static_cast<std::uint8_t>(samples);
    pattern.grid = alike ? 1 : 2;
    for (std::uint32_t pixel = 0; pixel < (alike ? 1u : 4u); ++pixel) {
        for (std::uint32_t sample = 0; sample < samples; ++sample) {
            pattern.positions[pixel * samples + sample] = position(pixel, sample);
        }
    }
    return pattern;
}

/// NEO-GTS-511: a 2-sample pattern as the pixel shaders' positions table (in
/// PushData::quad_sample_mask): byte pixel * 2 + sample, pixel = x + 2 * y in the 2x2 quad, each
/// a position as in Pattern. Another count (a pipeline without the state) gives the standard ones.
inline std::array<std::uint32_t, 2> QuadTable(const Pattern& pattern) {
    const Pattern used = pattern.samples == 2 ? pattern : Standard(2);
    std::array<std::uint32_t, 2> table{};
    for (std::uint32_t i = 0; i < 8; ++i) {
        const std::uint32_t byte = used.positions[used.grid == 2 ? i : i % 2];
        table[i / 4] |= byte << ((i % 4) * 8);
    }
    return table;
}

/// The Vulkan structure of a pattern; `storage` holds its locations.
inline vk::SampleLocationsInfoEXT ToVulkan(const Pattern& pattern,
                                           std::array<vk::SampleLocationEXT, 64>& storage) {
    const std::uint32_t count = std::min<std::uint32_t>(pattern.Count(), 64);
    for (std::uint32_t i = 0; i < count; ++i) {
        storage[i] = vk::SampleLocationEXT{
            .x = static_cast<float>(pattern.positions[i] & 0xf) / 16.0f,
            .y = static_cast<float>(pattern.positions[i] >> 4) / 16.0f,
        };
    }
    return vk::SampleLocationsInfoEXT{
        .sampleLocationsPerPixel = static_cast<vk::SampleCountFlagBits>(pattern.samples),
        .sampleLocationGridSize = vk::Extent2D{pattern.grid, pattern.grid},
        .sampleLocationsCount = count,
        .pSampleLocations = storage.data(),
    };
}

} // namespace Vulkan::SampleLocations
