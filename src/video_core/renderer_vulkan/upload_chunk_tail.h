// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>

namespace Vulkan {
// Append-only upload storage may span command lists. Published offsets never
// change; a full chunk retires behind the last list that can reference it.
template<class Chunks, class Retired>
void RetireUploadChunks(Chunks& chunks, Retired& retired, std::uint64_t& used,
                        std::uint64_t tick, std::uint64_t standard_size) {
    const bool keep = !chunks.empty() && chunks.back().buffer->Size() == standard_size &&
                      used < standard_size;
    const auto count = chunks.size() - (keep ? 1 : 0);
    for (std::size_t i = 0; i < count; ++i) {
        chunks[i].retire_tick = tick;
        retired.push_back(std::move(chunks[i]));
    }
    if (keep) {
        if (chunks.size() > 1) chunks.front() = std::move(chunks.back());
        chunks.resize(1);
        chunks.front().retire_tick = tick;
    } else {
        chunks.clear();
        used = 0;
    }
}
} // namespace Vulkan
