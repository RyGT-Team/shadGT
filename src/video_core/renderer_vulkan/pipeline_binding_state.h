// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include "common/diagnostics.h"

namespace Vulkan {
// Graphics and compute bindings are independent command-buffer state. External
// helper commands conservatively invalidate both; a new command buffer does too.
template <typename Pipeline, bool TrackCounts = Common::DiagnosticsBuilt>
struct PipelineBindingState {
    std::array<Pipeline, 2> handles{};
    std::array<uint64_t, 2> emitted{}, reused{};

    // Functional epoch for dependent command-state memos. It advances even
    // when diagnostic counters are compiled out.
    void Invalidate() { handles = {}; ++invalidation_epoch; }
    uint64_t InvalidationEpoch() const { return invalidation_epoch; }

    template <typename Emit>
    void Graphics(Pipeline pipeline, Emit&& emit) {
        Bind(0, pipeline, false, emit);
    }
    template <typename Emit>
    void Compute(Pipeline pipeline, bool force, Emit&& emit) {
        Bind(1, pipeline, force, emit);
    }
    void ResetCounts() { emitted = {}; reused = {}; }

private:
    uint64_t invalidation_epoch{};
    template <typename Emit>
    void Bind(unsigned domain, Pipeline pipeline, bool force, Emit& emit) {
        if (force || handles[domain] != pipeline) {
            emit();
            handles[domain] = pipeline;
            if constexpr (TrackCounts) ++emitted[domain];
        } else {
            if constexpr (TrackCounts) ++reused[domain];
        }
    }
};
}
