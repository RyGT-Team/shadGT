// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <numeric>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Native {
template <class Part>
void TransferLinearDownloadLeases(std::vector<std::shared_ptr<void>>& stream,
                                  std::vector<Part>& parts) {
    for (auto& part : parts) stream.push_back(std::move(part.lease));
}
template <class Plans, class Queue, class Overlaps>
void CancelOverlappingLinearPlans(Plans& plans, Queue& queue, Overlaps&& overlaps) {
    std::erase_if(plans, [&](const auto& item) {
        if (!overlaps(item.first)) return false;
        std::erase(queue, item.first);
        return true;
    });
}
constexpr bool LinearDownloadOverlaps(std::uint64_t a, std::uint64_t a_size,
                                      std::uint64_t b, std::uint64_t b_size) {
    if (!a_size || !b_size) return false;
    return a <= b ? b - a < a_size : a - b < b_size;
}
struct LinearImageShape { std::uint32_t width, height, depth, mips, layers, samples; };
constexpr bool LinearImageShapeMatches(LinearImageShape guest, LinearImageShape actual, bool is_2d) {
    return is_2d && guest.width && guest.height && guest.depth == 1 && guest.mips &&
           guest.layers && guest.samples == 1 && actual.width == guest.width &&
           actual.height == guest.height && actual.depth == guest.depth &&
           actual.mips == guest.mips && actual.layers == guest.layers && actual.samples == guest.samples;
}
struct LinearDownloadPart { std::uint64_t range, size, offset, capacity; };
struct LinearDownloadMip { std::uint64_t offset; std::uint32_t pitch, rows, width, height, layers; };
struct LinearDownloadCopy {
    std::size_t part;
    std::uint64_t buffer_offset, range_offset;
    std::uint32_t mip, layer, x, y, width, height, pitch;
};

// Plan only actual pixels. The imported parts must cover the complete guest
// allocation, but padding between pixels is deliberately never written.
inline bool PlanLinearImageDownload(std::span<const LinearDownloadMip> mips,
                                    std::span<const LinearDownloadPart> parts,
                                    std::uint64_t total, std::uint32_t texel_bytes,
                                    std::vector<LinearDownloadCopy>& copies) {
    copies.clear();
    constexpr auto Max = ~std::uint64_t{0};
    if (!total || !texel_bytes || mips.empty() || parts.empty()) return false;
    std::uint64_t covered = 0;
    for (const auto& part : parts) {
        if (!part.size || part.range != covered || part.offset > part.capacity ||
            part.size > part.capacity - part.offset || part.size > total - covered) return false;
        covered += part.size;
    }
    if (covered != total) return false;
    const std::uint64_t alignment = std::lcm<std::uint64_t>(4, texel_bytes);
    std::uint64_t previous_end = 0;
    for (std::uint32_t mip = 0; mip < mips.size(); ++mip) {
        const auto& m = mips[mip];
        if (!m.width || !m.height || !m.layers || m.width > 0x7fffffffu || m.height > 0x7fffffffu ||
            m.width > m.pitch || m.height > m.rows ||
            std::uint64_t(m.width) * texel_bytes % 4) return false;
        const auto final_row = std::uint64_t(m.layers - 1) * m.rows + m.height - 1;
        if (m.offset < previous_end || final_row > Max / m.pitch ||
            final_row * m.pitch > (Max - m.offset) / texel_bytes) return false;
        const auto last_start = m.offset + final_row * m.pitch * texel_bytes;
        const auto final_bytes = std::uint64_t(m.width) * texel_bytes;
        if (last_start > total || final_bytes > total - last_start) return false;
        previous_end = last_start + final_bytes;
        for (std::uint32_t layer = 0; layer < m.layers; ++layer) {
            for (std::uint32_t y = 0; y < m.height; ++y) {
                const std::uint64_t row = std::uint64_t(layer) * m.rows + y;
                if (row > Max / m.pitch || row * m.pitch > (Max - m.offset) / texel_bytes)
                    return false;
                const std::uint64_t start = m.offset + row * m.pitch * texel_bytes;
                const std::uint64_t row_bytes = std::uint64_t(m.width) * texel_bytes;
                if (start > total || row_bytes > total - start) return false;
                const std::uint64_t end = start + row_bytes;
                auto position = start;
                while (position < end) {
                    const auto it = std::upper_bound(parts.begin(), parts.end(), position,
                        [](std::uint64_t value, const auto& part) { return value < part.range; });
                    if (it == parts.begin()) return false;
                    const auto index = std::size_t((it - parts.begin()) - 1);
                    const auto& part = parts[index];
                    const auto stop = std::min(end, part.range + part.size);
                    const auto length = stop - position;
                    const auto buffer_offset = part.offset + position - part.range;
                    if (!length || buffer_offset % alignment || length % 4 || length % texel_bytes)
                        return false;
                    const auto x = std::uint32_t((position - start) / texel_bytes);
                    const auto width = std::uint32_t(length / texel_bytes);
                    if (!copies.empty()) {
                        auto& prior = copies.back();
                        const auto previous_rows = std::uint64_t(prior.height) * m.pitch;
                        if (x == 0 && width == m.width && prior.part == index && prior.mip == mip &&
                            prior.layer == layer && prior.x == 0 && prior.width == width &&
                            prior.y + prior.height == y &&
                            previous_rows <= (Max - prior.buffer_offset) / texel_bytes &&
                            prior.buffer_offset + previous_rows * texel_bytes == buffer_offset) {
                            ++prior.height;
                            position = stop;
                            continue;
                        }
                    }
                    copies.push_back({index, buffer_offset, position, mip, layer, x, y,
                                      width, 1, m.pitch});
                    position = stop;
                }
            }
        }
    }
    return !copies.empty();
}
}
