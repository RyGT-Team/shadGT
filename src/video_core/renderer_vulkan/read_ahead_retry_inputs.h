// Copyright 2026 RyGT team. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstring>
#include <span>
#include <vector>
#include "video_core/amdgpu/read_ahead_states.h"

namespace Vulkan {
inline std::vector<u32> CaptureRetryRegisters(std::span<const u32> regs) {
    using Ranges = AmdGpu::ReadAheadStates;
    if (regs.size() < Ranges::RegisterFileDwords) return {};
    std::vector<u32> out(Ranges::StateDwords);
    size_t offset = 0;
    for (const auto& range : Ranges::Ranges) {
        std::memcpy(out.data() + offset, regs.data() + range.first, range.count * sizeof(u32));
        offset += range.count;
    }
    return out;
}
inline bool RestoreRetryRegisters(std::span<u32> regs, std::span<const u32> saved) {
    using Ranges = AmdGpu::ReadAheadStates;
    if (regs.size() < Ranges::RegisterFileDwords || saved.size() != Ranges::StateDwords) return false;
    size_t offset = 0;
    for (const auto& range : Ranges::Ranges) {
        std::memcpy(regs.data() + range.first, saved.data() + offset, range.count * sizeof(u32));
        offset += range.count;
    }
    return true;
}
} // namespace Vulkan
