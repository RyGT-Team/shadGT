#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace Vulkan {
inline constexpr std::size_t DriverCacheDiskBudget = 2ull << 30;
inline constexpr std::uint64_t DriverCacheCommitReserve = 1ull << 30;
inline std::size_t DriverCacheSnapshotBudget(std::size_t reported, std::uint64_t available) {
    if (available <= DriverCacheCommitReserve) return 0;
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        std::min(reported, DriverCacheDiskBudget), available - DriverCacheCommitReserve));
}
// Vulkan guarantees that even VK_INCOMPLETE returns valid initial cache data.
// Never truncate a previously serialized blob: ask the driver for a bounded one.
template<class Read> std::vector<std::uint8_t> ReadBoundedDriverCache(Read&& read,
                                                                   std::uint64_t available) {
    std::size_t reported{};
    if (read(&reported, nullptr) != 0) return {};
    const auto budget = DriverCacheSnapshotBudget(reported, available);
    if (budget < 32) return {};
    std::vector<std::uint8_t> data(budget);
    auto written = budget;
    const int result = read(&written, data.data());
    if ((result != 0 && result != 5) || written < 32 || written > budget) return {};
    data.resize(written);
    return data;
}
}
