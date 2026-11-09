// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <utility>

namespace Vulkan {
// Called under the pipeline lookup lock. Base and compute-only scans are each done once per
// program code hash; runtime workgroup and translator policy are applied by the caller.
class ShaderFixScanCache {
public:
    struct Entry {
        std::uint32_t base_fixes{};
        bool base_scanned{}, compute_scanned{}, lane_reads{}, lane_reads_in_loop{};
    };

    template <typename BaseScan, typename ComputeScan>
    const Entry* GetIfNeeded(std::uint64_t hash, bool first_use, bool legacy, bool compute,
                             std::size_t stored_boundary, BaseScan&& base, ComputeScan&& extra) {
        if (!legacy && (stored_boundary == 0 || (!first_use && !compute))) return nullptr;
        return &Get(hash, compute, std::forward<BaseScan>(base), std::forward<ComputeScan>(extra));
    }

    template <typename BaseScan, typename ComputeScan>
    const Entry& Get(std::uint64_t hash, bool compute, BaseScan&& base, ComputeScan&& extra) {
        auto& entry = entries[hash];
        if (!entry.base_scanned) {
            entry.base_fixes = base();
            entry.base_scanned = true;
        }
        if (compute && !entry.compute_scanned) {
            const auto [reads, loop] = extra();
            entry.lane_reads = reads;
            entry.lane_reads_in_loop = loop;
            entry.compute_scanned = true;
        }
        return entry;
    }
private:
    std::unordered_map<std::uint64_t, Entry> entries;
};
} // namespace Vulkan
