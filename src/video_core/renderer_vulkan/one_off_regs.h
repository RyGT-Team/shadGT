// Copyright 2026 RyGT team. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_vulkan/one_off_observer.h"

namespace Vulkan {
struct OneOffOutputs {
    std::array<OneOffKey, AmdGpu::NUM_COLOR_BUFFERS + 1> keys{};
    std::size_t count{};
};

inline OneOffOutputs GetOneOffOutputs(const AmdGpu::Regs& regs) {
    OneOffOutputs result;
    if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable) {
        for (u32 i = 0; i < AmdGpu::NUM_COLOR_BUFFERS; ++i) {
            const auto& cb = regs.color_buffers[i];
            if (!cb || !regs.color_target_mask.GetMask(i)) continue;
            result.keys[result.count++] = {cb.Address(), cb.Pitch(), cb.Height(), cb.BaseSlice(),
                cb.NumSlices(), cb.NumSamples(), cb.info.raw, cb.attrib.raw, 0, false};
        }
    }
    const auto& db = regs.depth_buffer;
    if (regs.depth_control.depth_enable && regs.depth_control.depth_write_enable && db.DepthWriteValid()) {
        result.keys[result.count++] = {db.DepthWriteAddress(), db.Pitch(), db.Height(),
            regs.depth_view.slice_start, regs.depth_view.NumSlices(), db.NumSamples(),
            u32(db.z_info.format), 0, u32(db.GetTileMode()), true};
    }
    return result;
}
} // namespace Vulkan
