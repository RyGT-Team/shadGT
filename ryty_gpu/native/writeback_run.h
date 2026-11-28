// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace Native {
template<class Part>
bool ExtendShaderWriteback(const Part& first, std::uint64_t bytes, const Part& next) {
    constexpr std::uint64_t limit = 4ull << 20;
    if (!bytes || !next.size || (bytes % 16) || (next.size % 16) ||
        bytes > limit || next.size > limit - bytes ||
        first.region_begin != next.region_begin || first.region_buffer != next.region_buffer ||
        first.before_index != next.before_index ||
        first.guest->NativeHandle() == 0 || first.guest->NativeHandle() != next.guest->NativeHandle()) {
        return false;
    }
    const auto follows = [bytes](std::uint64_t start, std::uint64_t offset) {
        return offset >= start && offset - start == bytes;
    };
    const auto capacity = first.guest->Size();
    return follows(first.region_offset, next.region_offset) &&
           follows(first.before_offset, next.before_offset) &&
           follows(first.guest_offset, next.guest_offset) &&
           first.guest_offset <= capacity && bytes + next.size <= capacity - first.guest_offset;
}
} // namespace Native
