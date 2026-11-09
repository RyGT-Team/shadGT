// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <iterator>
#include <optional>
#include <vector>
namespace VideoCore {
// Called with the import map locked. Return no parts on any hole or alignment error.
template<class Part, class Map, class Make>
std::optional<std::vector<Part>> PlanImportedParts(const Map& views, std::uint64_t address,
    std::uint64_t size, std::uint64_t alignment, std::uint64_t size_alignment, Make make) {
    if (!size || !alignment || !size_alignment || address > UINT64_MAX - size) return std::nullopt;
    std::vector<Part> parts;
    const auto end = address + size;
    for (auto cursor = address; cursor < end;) {
        const auto it = views.upper_bound(cursor);
        if (it == views.begin()) return std::nullopt;
        const auto& view = std::prev(it)->second;
        if (cursor < view.begin || cursor >= view.end || !view.wrapper || !view.lease) return std::nullopt;
        const auto offset = cursor - view.begin, bytes = std::min(end, view.end) - cursor;
        if (offset % alignment || bytes % size_alignment) return std::nullopt;
        parts.push_back(make(view, offset, cursor - address, bytes));
        cursor += bytes;
    }
    return parts;
}
}
