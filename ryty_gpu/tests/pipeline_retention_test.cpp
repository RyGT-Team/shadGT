#include "video_core/renderer_vulkan/pipeline_retention_budget.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
int main() {
    using Vulkan::PipelineRetentionUnderPressure;
    constexpr std::uint64_t GiB = 1ull << 30;
    std::uint64_t cases = 0;
    for (std::uint64_t mib = 0; mib <= 32768; ++mib) {
        const auto budget = mib << 20;
        const auto threshold = budget > 2 * GiB ? budget - 2 * GiB : 0;
        assert(!PipelineRetentionUnderPressure(0, budget));
        assert(!PipelineRetentionUnderPressure(threshold, budget));
        assert(PipelineRetentionUnderPressure(threshold + 1, budget) == (budget != 0));
        if (threshold) assert(!PipelineRetentionUnderPressure(threshold - 1, budget));
        cases += 4;
    }
    assert(!PipelineRetentionUnderPressure(UINT64_MAX, 0));
    assert(PipelineRetentionUnderPressure(UINT64_MAX, UINT64_MAX));
    assert(!PipelineRetentionUnderPressure(UINT64_MAX - 2 * GiB, UINT64_MAX));
    // FIX-GTS-468: the system RAM reserve is total/8, clamped to 1..2 GiB.
    using Vulkan::SystemRamUnderPressure;
    assert(!SystemRamUnderPressure(0, 0));
    assert(SystemRamUnderPressure(2 * GiB - 1, 16 * GiB));
    assert(!SystemRamUnderPressure(2 * GiB, 16 * GiB));
    assert(SystemRamUnderPressure(2 * GiB - 1, 64 * GiB));
    assert(!SystemRamUnderPressure(2 * GiB, 64 * GiB));
    assert(SystemRamUnderPressure(GiB - 1, 4 * GiB));
    assert(!SystemRamUnderPressure(GiB, 4 * GiB));
    assert(SystemRamUnderPressure(GiB + GiB / 2 - 1, 12 * GiB));
    assert(!SystemRamUnderPressure(GiB + GiB / 2, 12 * GiB));
    printf("PASS %llu pipeline budget boundary cases\n", (unsigned long long)(cases + 12));
}
