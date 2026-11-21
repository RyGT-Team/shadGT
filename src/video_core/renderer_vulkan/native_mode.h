// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Vulkan {

#ifdef RYTY_GPU
// The renderer selected at startup by SHADGT_NATIVE or the game profile.
bool NativeModeRequested();
#else
constexpr bool NativeModeRequested() {
    return false;
}
#endif

} // namespace Vulkan
