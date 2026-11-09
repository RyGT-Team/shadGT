#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace Native::Parallel {
// The caller must separately prove that the table upload and its mapped backing
// remain valid for the ordered submission. This proves only the selected slot.
inline bool IsPreparedDirectSlot(std::span<const std::byte> bytes, std::size_t count,
                                 std::size_t index, bool guest_readonly_unformatted,
                                 bool null_buffer, std::uint64_t offset,
                                 std::uint64_t range) noexcept {
    constexpr auto Max = static_cast<std::size_t>(-1);
    if (!guest_readonly_unformatted || !null_buffer || offset != 0 ||
        range != UINT64_MAX || index >= count || count > (Max - 16) / 16 ||
        bytes.size() != 16 + count * 16) return false;
    std::uint32_t words[4];
    std::memcpy(words, bytes.data() + 16 + index * 16, sizeof(words));
    const auto base = std::uint64_t(words[0]) | (std::uint64_t(words[1]) << 32);
    const auto size = std::uint64_t(words[2]);
    constexpr std::uint64_t Limit = std::uint64_t{1} << 40;
    return words[3] == 1 && base != 0 && size != 0 && base < Limit && size <= Limit - base;
}
}