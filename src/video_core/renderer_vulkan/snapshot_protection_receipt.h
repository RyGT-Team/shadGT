// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <limits>

namespace Vulkan::SnapshotLease {

// Normalize the platform query without weakening protection checks. Unknown
// types, protections and failed queries must be reported as Other.
enum class ReceiptAccess : std::uint8_t { Other, ReadOnly, ReadWrite };
struct ReceiptRegion {
    std::uint64_t begin{}, size{};
    bool committed_mapped{};
    ReceiptAccess access{ReceiptAccess::Other};
};

// Caller must hold a registered, readable range pin and fill_watch_mutex from
// before this call through capture. The pin protocol must exclude ALL guest
// mapping/protection changes; native watch mutations use page-manager locks.
// CPU/IO/GPU writers must poison before opening/writing. The callbacks must not
// allocate GPU resources or wait for GPU work. No lease is certified here.
// Failed calls may have installed a watch; its owner must retire it normally.
template <class Query, class ProvesNonExec, class ProvesNoP2, class Watch, class Reapply>
bool ProtectAndVerify(std::uint64_t begin, std::uint64_t size, Query&& query,
                      ProvesNonExec&& proves_non_exec, ProvesNoP2&& proves_no_p2,
                      Watch&& watch, Reapply&& reapply) {
    constexpr unsigned MaxRegions = 512;
    constexpr std::uint64_t PageSize = 4096;
    if (!size || begin % PageSize || size % PageSize ||
        size > std::numeric_limits<std::uint64_t>::max() - begin)
        return false;
    const auto end = begin + size;
    const auto covers = [&](bool readonly) {
        auto cursor = begin;
        unsigned regions = 0;
        while (cursor < end) {
            if (++regions > MaxRegions)
                return false;
            ReceiptRegion region{};
            if (!query(cursor, region) || !region.committed_mapped ||
                region.begin > cursor || !region.size ||
                region.size > std::numeric_limits<std::uint64_t>::max() - region.begin)
                return false;
            const auto limit = region.begin + region.size;
            if (limit <= cursor ||
                (region.access != ReceiptAccess::ReadOnly &&
                 (readonly || region.access != ReceiptAccess::ReadWrite)))
                return false;
            cursor = limit < end ? limit : end;
        }
        return true;
    };
    if (!covers(false) || !proves_non_exec(begin, size) || !proves_no_p2(begin, size))
        return false;
    if (!watch(begin, size))
        return false;
    if (covers(true))
        return true;
    if (!reapply(begin, size))
        return false;
    return covers(true);
}

} // namespace Vulkan::SnapshotLease
