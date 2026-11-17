#pragma once
#include <algorithm>
#include <cstdint>

namespace Vulkan {
// Saturating subtraction also handles devices with a budget below the reserve.
// FIX-GTS-521: the reserve is an eighth of the budget, between 512 MB and 2 GB (2 GB flat before).
// On an 8 GB GPU (7.2 GB budget) GT7 passes budget - 2 GB with its textures alone after a few
// minutes, so the trim retired pipelines all session for little VRAM (pipelines are mostly in
// system RAM): the scenery of each lap lost its pipelines and popped back in.
constexpr std::uint64_t PipelineRetentionReserve(std::uint64_t budget) {
    constexpr std::uint64_t MiB = 1ull << 20;
    return budget / 8 < 512 * MiB ? 512 * MiB : budget / 8 > 2048 * MiB ? 2048 * MiB : budget / 8;
}

constexpr bool PipelineRetentionUnderPressure(std::uint64_t usage, std::uint64_t budget) {
    const std::uint64_t reserve = PipelineRetentionReserve(budget);
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
