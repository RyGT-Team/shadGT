// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Vulkan {
// Explicit OFF always wins. Diagnostic A/B alternates only in diagnostic builds.
constexpr bool NativeRingWindow(bool enabled, bool diagnostics, bool compare, std::uint64_t window) {
    return enabled && (!diagnostics || !compare || window % 2 != 0);
}

constexpr bool NativeBeginAheadAllowed(bool reuse, bool intel, bool parallel, bool worker,
                                       bool has_list, bool ring_available) {
    return reuse && !intel && !parallel && !worker && !has_list && !ring_available;
}
}
