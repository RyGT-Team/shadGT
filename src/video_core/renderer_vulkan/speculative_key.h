// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/recoverable.h"
#include "common/scope_exit.h"

namespace Vulkan {
// A speculative draw's registers can refer to recycled guest code. Only the recoverable
// failure is swallowed; the caller retains normal exception handling for all other failures.
template <typename Registers, typename Evaluate>
bool TrySpeculativeKey(const Registers*& current, const Registers* speculative, Evaluate&& evaluate) {
    const auto* previous = current;
    current = speculative;
    SCOPE_EXIT { current = previous; };
    try {
        Common::RecoverableScope recoverable;
        return evaluate();
    } catch (const Common::RecoverableFailure&) {
        return false;
    }
}
} // namespace Vulkan
