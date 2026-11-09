// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace Vulkan {
constexpr bool ShaderObjectPolicyAllows(bool enabled, std::uint32_t lds_emulation,
                                        std::uint32_t kinds, std::uint32_t skip) {
    return enabled && lds_emulation == 0 && (kinds & skip) == 0;
}
}
