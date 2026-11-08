// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace Vulkan {
// NVIDIA GTS grass was correct with unlinked objects in a matched visual comparison.
// An explicit override applies to every vendor.
constexpr bool ShaderObjectLinkingEnabled(std::uint32_t vendor, const char* override_value) {
    return override_value && *override_value ? *override_value != '0' : vendor != 0x10de;
}

constexpr bool ShaderObjectPolicyAllows(bool enabled, std::uint32_t lds_emulation,
                                        std::uint32_t kinds, std::uint32_t skip) {
    return enabled && lds_emulation == 0 && (kinds & skip) == 0;
}
}
