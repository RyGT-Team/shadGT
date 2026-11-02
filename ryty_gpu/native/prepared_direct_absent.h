#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
namespace Native::Parallel {
// Mode zero selects the ordinary descriptor fallback. Admit only a proven
// absent guest buffer with an actually enabled nullDescriptor feature; all
// other nulls and mode values remain on the normal recorder path.
inline bool IsPreparedAbsentDirectSlot(std::span<const std::byte> table,std::size_t count,std::size_t index,
    bool guest_readonly_unformatted,bool null_descriptor_enabled,bool null_buffer,
    std::uint64_t offset,std::uint64_t range,std::uint64_t sharp_base,std::uint64_t sharp_size) noexcept {
    constexpr auto Max=static_cast<std::size_t>(-1);
    if(!guest_readonly_unformatted || !null_descriptor_enabled || !null_buffer || offset || range!=UINT64_MAX ||
       (sharp_base && sharp_size) || index>=count || count>(Max-16)/16 || table.size()!=16+count*16)return false;
    std::uint32_t words[4]{};std::memcpy(words,table.data()+16+index*16,sizeof(words));
    return !(words[0] | words[1] | words[2] | words[3]);
}
}
