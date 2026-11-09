// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Native {
enum class HighCpuWriteback { NotNeeded, Existing, Admit, OverLimit, InsufficientBudget };

constexpr HighCpuWriteback ClassifyHighCpuWriteback(
    std::uint64_t address, std::uint64_t size, std::uint64_t inplace_from,
    std::uint64_t device_budget, std::uint64_t device_usage, bool enabled,
    std::uint64_t existing_limit = 0) {
    constexpr std::uint64_t max_size = 256ull << 20;
    if (size && size <= existing_limit) return HighCpuWriteback::Existing;
    if (!enabled || !size || address < inplace_from) return HighCpuWriteback::NotNeeded;
    if (size > max_size || size > ~std::uint64_t{0} - address)
        return HighCpuWriteback::OverLimit;
    if ((address | size) & 3u) return HighCpuWriteback::NotNeeded;
    // Worst case: scatter output plus two tiling buffers. Budget==0 means the
    // driver does not expose a meaningful budget; actual allocation still preflights.
    if (device_budget && (device_usage >= device_budget ||
        size > (device_budget - device_usage) / 3)) return HighCpuWriteback::InsufficientBudget;
    return HighCpuWriteback::Admit;
}
}
