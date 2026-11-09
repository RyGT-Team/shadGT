// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace Vulkan {
// Retry a binding pass when it replaced a region used earlier in the same pass.
template <typename Generation, typename Bind, typename Reset>
bool BindRegionsStable(Generation generation, Bind bind, Reset reset) {
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const auto before = generation();
        if (!bind()) return false;
        if (generation() == before) return true;
        reset();
    }
    return false;
}
} // namespace Vulkan
