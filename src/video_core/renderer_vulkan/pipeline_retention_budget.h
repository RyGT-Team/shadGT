#pragma once
#include <cstdint>

namespace Vulkan {
// Saturating subtraction also handles devices with a budget below the reserve.
constexpr bool PipelineRetentionUnderPressure(std::uint64_t usage, std::uint64_t budget) {
    constexpr std::uint64_t reserve = 2ull << 30;
    return budget != 0 && usage > (budget > reserve ? budget - reserve : 0);
}
} // namespace Vulkan
