// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdlib>
#include <cstdint>
#include <string_view>

namespace Shader {
constexpr bool LaneReadUnbranchPolicy(const char* value) noexcept {
    return !(value && value[0] == '0');
}

inline bool LaneReadUnbranchEnabled() noexcept {
    static const bool enabled = LaneReadUnbranchPolicy(std::getenv("RYGT_LANE_READ_UNBRANCH"));
    return enabled;
}

constexpr std::string_view LaneReadStoreSuffix(bool enabled) noexcept {
    return enabled ? "" : ", RYGT_LANE_READ_UNBRANCH=0";
}

constexpr std::uint32_t LaneReadFixMarker(bool compute, std::uint32_t subgroup_size,
                                           std::uint64_t workgroup_size, bool enabled) noexcept {
    return compute && subgroup_size < 64 && workgroup_size == 64 && enabled ? 2048u : 0u;
}
} // namespace Shader
