#pragma once
#include <cstdint>
#include <limits>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
namespace Native {
// Keys must be aligned to Page. Predicate must be the original scan predicate.
template <std::uint64_t Page, class Container, class Predicate>
void EraseAligned(Container& values, std::uint64_t address, std::uint64_t size,
                  bool overlap, bool enabled, Predicate predicate) {
    constexpr auto max = (std::numeric_limits<std::uint64_t>::max)();
    const auto scan = [&] { std::erase_if(values, predicate); };
    if (!enabled || size == 0 || size > max-address || address > max-(Page-1)) return scan();
    const auto end=address+size;
    if (end > max-Page) return scan(); // Preserve wrapping behavior in the original predicate.
    const auto first = overlap ? address & ~(Page-1) : (address+Page-1) & ~(Page-1);
    if (first >= end) return;
    const auto count=(end-1-first)/Page+1;
    if (count > values.size()/4) return scan();
    auto key=first;
    for (std::uint64_t i=0;i<count;++i,key+=Page) {
        const auto found=values.find(key);
        if (found != values.end() && predicate(*found)) values.erase(found);
    }
}
}
