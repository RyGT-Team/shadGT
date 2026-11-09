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
    printf("PASS %llu pipeline budget boundary cases\n", (unsigned long long)(cases + 3));
}
