// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>

namespace Vulkan {
// The lookup mutex serializes the map, draw promotion and this scan. The atomic claim also
// serializes cancellation with a worker starting the build.
template <typename Build>
bool CancelUnstartedPipeline(Build& build) {
    if (build.started.exchange(true)) return false;
    build.promise.set_value();
    return true;
}

template <typename Map, typename Key>
std::size_t ExpireStaleAheadBuilds(Map& pending, const Key& current,
                                   std::chrono::steady_clock::time_point now) {
    std::size_t cancelled = 0;
    for (auto it = pending.begin(); it != pending.end();) {
        auto& build = *it->second;
        if (it->first == current || build.preloading || build.async || build.urgent ||
            now - build.queued < std::chrono::seconds{30} || !CancelUnstartedPipeline(build)) {
            ++it;
            continue;
        }
        it = pending.erase(it);
        ++cancelled;
    }
    return cancelled;
}
} // namespace Vulkan
