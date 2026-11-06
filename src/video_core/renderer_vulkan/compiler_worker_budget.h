// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdlib>
namespace Vulkan {
struct CompilerWorkerBudget {
    // Concurrent tessellation compiles exhausted system commit in NVIDIA LLVM.
    // Share one admission limit between dispatch and direct driver calls.
    static constexpr unsigned HeavyPipelineLimit = 1;
    // AMD-321: AMD's compiler kept tessellation compiles in check (AMD-320 retains no
    // link-time data for them), so one at a time only queued the precompile behind them.
    // AMD admits a quarter of the logical processors, at most four;
    // RYGT_HEAVY_PIPELINE_LIMIT=1..16 overrides on any GPU.
    static unsigned& HeavyLimitStorage() {
        static unsigned limit = HeavyPipelineLimit;
        return limit;
    }
    static void SelectHeavyLimit(unsigned vendor_id, unsigned logical) {
        unsigned limit = vendor_id == 0x1002 ? std::clamp(logical / 4, 1u, 4u) : HeavyPipelineLimit;
        if (const char* env = std::getenv("RYGT_HEAVY_PIPELINE_LIMIT"); env && *env) {
            limit = std::clamp(unsigned(std::strtoul(env, nullptr, 10)), 1u, 16u);
        }
        HeavyLimitStorage() = limit;
    }
    static unsigned HeavyLimit() {
        return HeavyLimitStorage();
    }
    // Splash-screen work may use every logical processor; gameplay stays bounded.
    static constexpr unsigned StartupPipelineWorkers(unsigned logical) {
        return std::max(1u, logical);
    }
    unsigned translation;
    unsigned pipeline;
    static constexpr CompilerWorkerBudget ForLogicalThreads(unsigned logical) {
        // Logical CPUs share execution and cache resources. Reserve half for game,
        // recording and the driver; do not start an almost machine-wide compile storm.
        // Bound large machines too: each driver compile can hold substantial memory.
        const unsigned total = logical > 2 ? std::min(8u, logical / 2) : 1;
        const unsigned translation = total > 1 ? std::max(1u, total / 4) : 0;
        return {translation, total - translation};
    }
};
} // namespace Vulkan
