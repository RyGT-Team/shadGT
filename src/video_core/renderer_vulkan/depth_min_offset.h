// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// FIX-SGT-124 (RyGT#84): AMD's Windows driver stores depths just above a non-zero viewport
// minDepth as minDepth itself. GT7 draws its terrain with the depth range [2.937e-6, 1] and its
// far geometry lands 1e-5 to 3e-5 above that minimum: on AMD all of it came out at exactly
// 2.937e-6, so the fallback ground plane GT7 draws under the scenery with [0, 1] won the depth
// test (a grey band over Alsace Village) and fog and depth of field read the flattened depth
// (blocky fog). Measured on an RDNA2 iGPU (driver 26.10.1): the vertex shader's z/w was right,
// depth the pixel shader wrote itself was right, and starting the range at 0 removed the band.
//
// Starting the range at 0 alone moves those draws' depth by minDepth, which breaks what GT7
// builds on that offset (its lake water is drawn 2.937e-6 in front of the coplanar plane). So
// the vertex shader adds the offset itself, z += k * w with k = min / (max - min), and the
// viewport becomes [0, max - min]: (max - min) * (z / w + k) = min + (max - min) * z / w, the
// depth the game asked for, without a non-zero minDepth.
//
// SHADGT_DEPTH_MIN_OFFSET=0 turns it off, =1 turns it on with any driver (A/B tests).

#include <cstdlib>

#include "video_core/amdgpu/regs.h"

namespace Vulkan {

/// Ranges starting above this are left as they are: the minimums GT7 uses are 2.9e-6 to 1.2e-4.
constexpr float DepthMinOffsetLimit = 1e-3f;

/// The offset k the vertex shader adds (z += k * w) and the viewport compensates for, or 0 when
/// the draw's depth range stays as it is. `driver_default`: the driver needs it (AMD Windows).
/// The vertex shader's runtime info and the viewport both take it from here, so they agree.
inline float DepthMinOffset(const AmdGpu::Regs& regs, bool driver_default) {
    static const int forced = [] {
        const char* env = std::getenv("SHADGT_DEPTH_MIN_OFFSET");
        return env && *env ? (env[0] == '1' ? 1 : 0) : -1;
    }();
    if (forced == 0 || (forced < 0 && !driver_default)) {
        return 0.f;
    }
    if (regs.IsClipDisabled() || regs.clipper_control.clip_space != AmdGpu::ClipSpace::ZeroToW) {
        return 0.f;
    }
    const auto& vp_ctl = regs.viewport_control;
    float min_depth = 0.f;
    float max_depth = 0.f;
    bool any = false;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        if (vp.xscale == 0) {
            continue;
        }
        const float zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const float zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;
        // One offset for the whole draw: viewports with different depth ranges stay as they are.
        if (any && (zoffset != min_depth || zoffset + zscale != max_depth)) {
            return 0.f;
        }
        min_depth = zoffset;
        max_depth = zoffset + zscale;
        any = true;
    }
    if (!any || !(min_depth > 0.f) || min_depth >= DepthMinOffsetLimit ||
        !(max_depth > min_depth) || max_depth > 1.f) {
        return 0.f;
    }
    return min_depth / (max_depth - min_depth);
}

} // namespace Vulkan
