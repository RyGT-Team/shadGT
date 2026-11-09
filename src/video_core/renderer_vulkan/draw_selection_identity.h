// Copyright 2026 RyGT team. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Vulkan {
inline bool CanUseDrawSelection(bool selected_indirect, bool selected_quad,
                                std::uint16_t selected_vertex, std::uint32_t selected_instance,
                                bool requested_indirect, std::uint16_t requested_vertex,
                                std::uint32_t requested_instance, bool direct_allowed) {
    if (selected_indirect != requested_indirect) return false;
    if (requested_indirect) {
        return selected_vertex == requested_vertex && selected_instance == requested_instance;
    }
    return direct_allowed || selected_quad;
}
} // namespace Vulkan
