#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
namespace Native {
// Two registry-wide buffers (gather and untile), at most 384 MiB combined.
constexpr uint64_t UploadScratchCapacity(uint64_t required) {
    constexpr uint64_t Limit = 192ull << 20;
    constexpr uint64_t Step = 16ull << 20;
    if (!required || required > Limit) return 0;
    if (required <= (32ull << 20)) return std::bit_ceil(std::max<uint64_t>(required,4096));
    return ((required + Step - 1) / Step) * Step;
}
}
