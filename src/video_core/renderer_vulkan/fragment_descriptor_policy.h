// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace Vulkan {
// Keep stable fragment materials in immutable descriptor sets instead of pushing them on every
// draw. Made for NVIDIA, where repeated material pushes dominated command recording.
// AMD-PERF-002: AMD too. Its driver writes every pushed descriptor into the command buffer, so a
// race frame of 2,000-3,800 draws repeated that work thousands of times; the cache is plain Vulkan
// and was only gated to the GPU it was tested on. RYGT_NVIDIA_CACHED_FRAGMENT_SETS=0 turns it off
// on NVIDIA, RYGT_AMD_CACHED_FRAGMENT_SETS=0 on AMD.
constexpr bool CacheFragmentDescriptors(std::uint32_t vendor, bool native,
                                         const char* override_value,
                                         const char* amd_override = nullptr) {
    if (!native) return false;
    const auto off = [](const char* value) {
        return value && value[0] == '0' && value[1] == '\0';
    };
    if (vendor == 0x10de) return !off(override_value);
    if (vendor == 0x1002) return !off(amd_override);
    return false;
}
}
