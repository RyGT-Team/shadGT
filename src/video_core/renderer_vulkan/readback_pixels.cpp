// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>

#include "video_core/renderer_vulkan/readback_pixels.h"

namespace Vulkan {
namespace {

// ST.2084 EOTF, using the code width of the image that was copied.
template <u32 MaxCode>
const auto& PqNits() {
    static const auto lut = [] {
        constexpr float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 32.0f;
        constexpr float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 128.0f, c3 = 2392.0f / 128.0f;
        std::array<float, MaxCode + 1> values{};
        for (u32 i = 0; i <= MaxCode; ++i) {
            const float v = std::pow(static_cast<float>(i) / MaxCode, 1.0f / m2);
            const float ratio = std::max(v - c1, 0.0f) / std::max(c2 - c3 * v, 1e-6f);
            values[i] = 10000.0f * std::pow(ratio, 1.0f / m1);
        }
        return values;
    }();
    return lut;
}

// The screenshot tone curve maps 100 nits to SDR white, followed by sRGB encoding.
u8 EncodeSdr(float nits) {
    const float x = nits / 100.0f;
    const float linear = (2.0f * x) / (1.0f + x);
    const float srgb =
        linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return static_cast<u8>(std::clamp(srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
}

u8 SdrChannel(float nits) {
    if (nits <= 0.0f)
        return 0;
    if (nits >= 100.0f)
        return 255;

    // An entire interval with equal endpoints maps to the same byte. Evaluate the exact curve
    // at rounding boundaries so continuous video and one-off screenshots give identical values.
    constexpr u32 steps_per_nit = 256;
    static const auto lut = [] {
        std::array<u8, 100 * steps_per_nit + 1> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = EncodeSdr(static_cast<float>(i) / steps_per_nit);
        }
        return values;
    }();
    const auto index = static_cast<size_t>(nits * steps_per_nit);
    return lut[index] == lut[index + 1] ? lut[index] : EncodeSdr(nits);
}

template <u32 MaxCode, typename ReadRgb>
void ConvertPixels(std::span<const u8> source, std::span<u8> output, bool hdr_encoded,
                   ReadbackOrder order, ReadRgb read_rgb) {
    const float* pq = hdr_encoded ? PqNits<MaxCode>().data() : nullptr;
    const size_t red = order == ReadbackOrder::Rgba ? 0 : 2;
    for (size_t i = 0; i < source.size(); i += 4) {
        const auto [r, g, b] = read_rgb(source.data() + i);
        if (pq) {
            // Linear Rec.2020 to Rec.709 before tone mapping.
            const float rn = pq[r], gn = pq[g], bn = pq[b];
            output[i + red] = SdrChannel(1.6605f * rn - 0.5876f * gn - 0.0728f * bn);
            output[i + 1] = SdrChannel(-0.1246f * rn + 1.1329f * gn - 0.0083f * bn);
            output[i + 2 - red] = SdrChannel(-0.0182f * rn - 0.1006f * gn + 1.1187f * bn);
        } else {
            const auto unorm = [](u32 v) {
                return static_cast<u8>((v * 255 + MaxCode / 2) / MaxCode);
            };
            output[i + red] = unorm(r);
            output[i + 1] = unorm(g);
            output[i + 2 - red] = unorm(b);
        }
        output[i + 3] = 255;
    }
}

} // namespace

bool ConvertReadbackPixels(std::span<const u8> source, std::span<u8> output, vk::Format format,
                           bool hdr_encoded, ReadbackOrder order) {
    if (source.size() != output.size() || source.size() % 4 != 0)
        return false;
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb: {
        const bool bgra =
            format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
        ConvertPixels<255>(source, output, hdr_encoded, order, [bgra](const u8* p) {
            return std::array<u32, 3>{p[bgra ? 2 : 0], p[1], p[bgra ? 0 : 2]};
        });
        return true;
    }
    case vk::Format::eA2R10G10B10UnormPack32:
    case vk::Format::eA2B10G10R10UnormPack32: {
        const bool red_low = format == vk::Format::eA2B10G10R10UnormPack32;
        ConvertPixels<1023>(source, output, hdr_encoded, order, [red_low](const u8* p) {
            const u32 packed = u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24);
            const u32 low = packed & 1023, green = (packed >> 10) & 1023,
                      high = (packed >> 20) & 1023;
            return std::array<u32, 3>{red_low ? low : high, green, red_low ? high : low};
        });
        return true;
    }
    default:
        return false;
    }
}

} // namespace Vulkan
