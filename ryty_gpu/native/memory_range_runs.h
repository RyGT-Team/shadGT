// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace Native {
// Visit only the words touched by a half-open bit interval. Boundary masks keep
// unrelated bits intact, including when another thread clears the same word.
template<class Fn>
void VisitBitWords(std::uint64_t first, std::uint64_t end, Fn&& fn) {
    while (first < end) {
        const auto word = first / 64;
        const auto stop = std::min(end, (word + 1) * 64);
        const auto lo = unsigned(first % 64);
        const auto hi = unsigned(stop - word * 64);
        const auto mask = (~std::uint64_t{0} << lo) & (~std::uint64_t{0} >> (64 - hi));
        fn(word, mask);
        first = stop;
    }
}

// Resolve a second-level page table once per contiguous run, rather than once
// per page. The callback still writes every page in the original address order.
template<unsigned PageBits, unsigned TableBits, class Fn>
void VisitPageTableRuns(std::uint64_t begin, std::uint64_t end, Fn&& fn) {
    constexpr auto page = std::uint64_t{1} << PageBits;
    constexpr auto entries = std::uint64_t{1} << TableBits;
    while (begin < end) {
        const auto first = (begin >> PageBits) & (entries - 1);
        const auto remaining = (end - begin - 1) / page + 1;
        const auto count = std::min(remaining, entries - first);
        fn(begin, first, count);
        if (count == remaining) break;
        begin += count * page;
    }
}
}
