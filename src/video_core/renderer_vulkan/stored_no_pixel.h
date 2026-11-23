// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>

#include "shader_recompiler/runtime_info.h"

namespace Vulkan {

// FIX-SGT-116: a stored pipeline without a pixel shader. Its build still reads the pixel stage's
// runtime info, which at a draw without that stage is what an earlier draw left, and which isn't
// stored. Without a pixel shader the build takes from it: the rect and quad lists' helper shaders;
// sample shading (stored, in the multisample state); with shader objects on, whether a
// multisampled pipeline is drawn with them; and, with the clip distance emulation, a shader that
// discards what the vertex stage's clip distances cut off (not stored: see VertexStageMayClip).
// A record is taken when none of them can make its build differ from a draw's; it is installed
// with that runtime info cleared.
template <typename Sdata>
bool StoredBuildIgnoresPixelInfo(const Sdata& sdata, bool rect_or_quad_list, bool tessellated,
                                 bool shader_objects_multisampled) {
    return !rect_or_quad_list && !tessellated && !shader_objects_multisampled &&
           sdata.tcs.empty() && sdata.tes.empty() && !sdata.multisampling.sampleShadingEnable;
}

/// FIX-SGT-116: whether a draw's build of a pipeline without a pixel shader may have had the clip
/// distance emulation's discard shader with anything to discard. That shader is never stored, and
/// whether a draw made it depends on what an earlier draw left; it is made from the vertex stage's
/// outputs alone (EmitDiscardFragmentShader) and with no clip distance among them it discards
/// nothing. The build reads those outputs as a hardware vertex stage's: any other (a geometry
/// stage's export) is refused. Without the emulation (only NVIDIA's driver has it) there is none.
inline bool VertexStageMayClip(const Shader::RuntimeInfo& vertex, bool emulates_clip_distances) {
    if (!emulates_clip_distances) {
        return false;
    }
    if (vertex.hw_stage != Shader::HwStage::Vertex) {
        return true;
    }
    for (const auto& map : vertex.hw.vs.outputs) {
        for (const auto output : map) {
            if (output >= Shader::Output::ClipDist0 && output <= Shader::Output::ClipDist7) {
                return true;
            }
        }
    }
    return false;
}

/// SHADGT_PRECOMPILE_NO_PS=0: the startup precompile leaves every such pipeline to its first draw,
/// as before.
inline bool PrecompileWithoutPixelShaderEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADGT_PRECOMPILE_NO_PS");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

} // namespace Vulkan
