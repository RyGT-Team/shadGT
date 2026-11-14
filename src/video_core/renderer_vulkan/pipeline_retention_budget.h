#pragma once
#include <algorithm>
#include <cstdint>

namespace Vulkan {
// Saturating subtraction also handles devices with a budget below the reserve.
constexpr bool PipelineRetentionUnderPressure(std::uint64_t usage, std::uint64_t budget) {
    constexpr std::uint64_t reserve = 2ull << 30;
    return budget != 0 && usage > (budget > reserve ? budget - reserve : 0);
}

// FIX-GTS-468: driver pipeline memory is mostly system RAM. Retire unused pipelines too when
// free physical memory falls below an eighth of it, between 1 and 2 GB (2 GB at 16 GB).
// Unknown totals (0) never count as pressure.
constexpr bool SystemRamUnderPressure(std::uint64_t available, std::uint64_t total) {
    constexpr std::uint64_t GiB = 1ull << 30;
    if (total == 0) return false;
    const std::uint64_t reserve = std::clamp<std::uint64_t>(total / 8, GiB, 2 * GiB);
    return available < reserve;
}
} // namespace Vulkan
