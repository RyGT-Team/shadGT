#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
namespace Native::Parallel {
struct GuestImageRange {std::uint64_t address{},size{};};
inline bool ValidGuestImageRange(GuestImageRange r) noexcept {
    return r.size && r.address<=std::numeric_limits<std::uint64_t>::max()-r.size;
}
inline bool GuestImageRangesOverlap(GuestImageRange a,GuestImageRange b) noexcept {
    return a.address<=b.address ? b.address-a.address<a.size : a.address-b.address<b.size;
}
template<std::size_t N> bool WritableRangesDisjoint(const std::array<GuestImageRange,N>& ranges,std::size_t count) noexcept {
    if(count>N)return false;
    for(std::size_t i=0;i<count;++i){if(!ValidGuestImageRange(ranges[i]))return false;for(std::size_t j=0;j<i;++j)if(GuestImageRangesOverlap(ranges[i],ranges[j]))return false;}
    return true;
}
}
