#pragma once
#include <cstddef>
namespace Native::Parallel {
// A hint never changes the result; stale or absent indices fall back to the full scan.
template<class Sequence, class Predicate>
bool ReadonlyHintOrScan(const Sequence& items, std::size_t hint, Predicate&& predicate, bool& hit) {
    hit = hint < items.size() && predicate(items[hint]);
    if (hit) return true;
    for (const auto& item : items) if (predicate(item)) return true;
    return false;
}
}
