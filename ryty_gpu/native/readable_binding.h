// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
namespace Native {
// Only clip large read-only bindings that have neither GPU-owned region data
// nor a complete imported view. Zero means a null descriptor, not an upload.
template <typename HasRegion, typename Imported, typename Readable>
std::uint64_t ReadableBindingSize(std::uint64_t address, std::uint64_t size,
                                  bool writable, bool enabled, bool has_direct,
                                  HasRegion&& has_region, Imported&& imported, Readable&& readable) {
    if (!enabled || writable || !has_direct || size < (4ull << 20) ||
        has_region(address, size) || imported(address, size)) return size;
    return std::min<std::uint64_t>(readable(address, size), size);
}
}
