#pragma once
#include <cstdint>
#include <limits>
namespace Native::Parallel {
inline bool PreparedIndexRange(std::uint64_t base,std::uint32_t first,std::uint32_t count,
    std::uint64_t element,std::uint64_t address,std::uint64_t offset,std::uint64_t& bytes) noexcept {
    if(element!=2 && element!=4)return false;
    const auto limit=std::numeric_limits<std::uint64_t>::max();
    const auto delta=std::uint64_t(first)*element;bytes=std::uint64_t(count)*element;
    return bytes && base<=limit-delta && address==base+delta && address<=limit-bytes &&
        !(offset%element) && offset<=limit-bytes;
}
inline bool PreparedIndexBacking(std::uint64_t address,std::uint64_t bytes,std::uint64_t begin,
    std::uint64_t end,std::uint64_t offset,std::uint64_t capacity) noexcept {
    if(!bytes || begin>end || address<begin || address>end || bytes>end-address)return false;
    const auto expected=address-begin;
    return offset==expected && offset<=capacity && bytes<=capacity-offset;
}
}
