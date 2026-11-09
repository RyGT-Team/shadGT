// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <semaphore>
#include <new>
#include <system_error>
namespace Vulkan {
std::counting_semaphore<64>& HeavyPipelineGate();
// Extra catch-up workers are optional. Existing workers remain owned if a later creation fails.
template <typename Workers, typename Factory>
bool TryGrowCompileWorkers(Workers& workers, unsigned width, Factory&& factory) {
    try {
        while (workers.size() < width) factory();
        return true;
    } catch (const std::system_error&) {
        return false;
    } catch (const std::bad_alloc&) {
        return false;
    }
}
struct CompilerWorkerBudget {
    // Concurrent tessellation compiles exhausted system commit in NVIDIA LLVM.
    // Share one admission limit between dispatch and direct driver calls.
    static constexpr unsigned HeavyPipelineLimit = 1;
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
