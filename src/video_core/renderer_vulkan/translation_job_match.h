// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Vulkan {
// A shared shader hash is not enough to identify the compilation this command
// depends on. Other stages/runtime permutations can still be queued independently.
template <class Job, class HwStage, class SwStage, class Runtime, class Bindings>
bool MatchesTranslationRequest(const Job& job, HwStage hw_stage, SwStage sw_stage,
                               const Runtime& runtime_info, const Bindings& start) {
    return job.hw_stage == hw_stage && job.sw_stage == sw_stage &&
           job.runtime_info == runtime_info && job.start == start;
}
}
