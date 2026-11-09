// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <utility>
#include <vector>

namespace VideoCore {
// Defer must execute callbacks only after the corresponding scheduler tick is
// GPU-complete. No release runs on the caller or while it holds the view lock.
// Small batches let other scheduler maintenance run between release groups.
template<class Record, class Defer, class Release>
void DeferImportReleases(std::vector<Record> records, Defer&& defer, Release release) {
    constexpr std::size_t BatchSize = 8;
    for (std::size_t first = 0; first < records.size(); first += BatchSize) {
        std::vector<Record> batch;
        const auto count = std::min(BatchSize, records.size() - first);
        batch.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            batch.push_back(std::move(records[first + i]));
        }
        defer([batch = std::move(batch), release]() mutable {
            for (auto& record : batch) release(record);
        });
    }
}
} // namespace VideoCore
