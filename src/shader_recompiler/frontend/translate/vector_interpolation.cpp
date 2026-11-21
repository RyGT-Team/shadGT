// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/profile.h"

namespace Shader::Gcn {

using Interpolation = Info::Interpolation;

static Interpolation GetInterpolation(IR::Attribute attribute) {
    switch (attribute) {
    case IR::Attribute::BaryCoordNoPersp:
        return {Qualifier::NoPerspective, Qualifier::None};
    case IR::Attribute::BaryCoordNoPerspCentroid:
        return {Qualifier::NoPerspective, Qualifier::Centroid};
    case IR::Attribute::BaryCoordNoPerspSample:
        return {Qualifier::NoPerspective, Qualifier::Sample};
    case IR::Attribute::BaryCoordSmooth:
        return {Qualifier::Smooth, Qualifier::None};
    case IR::Attribute::BaryCoordSmoothCentroid:
        return {Qualifier::Smooth, Qualifier::Centroid};
    case IR::Attribute::BaryCoordSmoothSample:
        return {Qualifier::Smooth, Qualifier::Sample};
    case IR::Attribute::BaryCoordPullModel:
        return {Qualifier::Smooth, Qualifier::None};
    default:
        UNREACHABLE_MSG("Unhandled barycentric attribute {}", NameOf(attribute));
    }
}

static constexpr std::array DefaultValTable = {
    std::array{0.0f, 0.0f, 0.0f, 0.0f},
    std::array{0.0f, 0.0f, 0.0f, 1.0f},
    std::array{1.0f, 1.0f, 1.0f, 0.0f},
    std::array{1.0f, 1.0f, 1.0f, 1.0f},
};

/// True when the fragment shader can read each vertex's value of an input (AMD explicit vertex
/// parameter or KHR fragment shader barycentric). NVIDIA Pascal has neither.
static bool HasPerVertexInputs(const Profile& profile) {
    return profile.supports_amd_shader_explicit_vertex_parameter ||
           profile.supports_fragment_shader_barycentric;
}

/// Coarse screen-space slopes of an interpolated value: the change one pixel to the right (x)
/// and one pixel down (y). Quad lanes 0/1 are horizontal neighbours, 0/2 vertical ones.
static std::pair<IR::F32, IR::F32> QuadDeltas(IR::IREmitter& ir, const IR::F32& value) {
    const IR::U32 bits = ir.BitCast<IR::U32>(value);
    const auto lane = [&](u32 index) {
        return ir.BitCast<IR::F32>(ir.QuadBroadcast(bits, ir.Imm32(index)));
    };
    const IR::F32 q0 = lane(0U);
    return {ir.FPSub(lane(1U), q0), ir.FPSub(lane(2U), q0)};
}

void Translator::EmitVectorInterpolation(const GcnInst& inst) {
    switch (inst.opcode) {
        // VINTRP
    case Opcode::V_INTERP_P1_F32:
        return V_INTERP_P1_F32(inst);
    case Opcode::V_INTERP_P2_F32:
        return V_INTERP_P2_F32(inst);
    case Opcode::V_INTERP_MOV_F32:
        return V_INTERP_MOV_F32(inst);
    default:
        LogMissingOpcode(inst);
    }
}

// VINTRP

void Translator::V_INTERP_P1_F32(const GcnInst& inst) {
    if (!profile.needs_manual_interpolation || !HasPerVertexInputs(profile)) {
        return;
    }
    const u32 attr_index = inst.control.vintrp.attr;
    const auto& attr = runtime_info.hw.fs.inputs[attr_index];
    if (attr.IsDefault()) {
        return;
    }
    // VDST = P10 * VSRC + P0
    const IR::Attribute attrib = IR::Attribute::Param0 + attr_index;
    const IR::F32 p0 = ir.GetAttribute(attrib, inst.control.vintrp.chan, 0);
    const IR::F32 p1 = ir.GetAttribute(attrib, inst.control.vintrp.chan, 1);
    const IR::F32 i = GetSrc<IR::F32>(inst.src[0]);
    const IR::F32 result = ir.FPFma(ir.FPSub(p1, p0), i, p0);
    SetDst(inst.dst[0], result);
}

void Translator::V_INTERP_P2_F32(const GcnInst& inst) {
    const u32 attr_index = inst.control.vintrp.attr;
    const IR::Attribute attrib = IR::Attribute::Param0 + attr_index;
    const auto& attr = runtime_info.hw.fs.inputs[attr_index];
    auto& interp = info.fs_interpolation[attr_index];
    if (attr.IsDefault()) {
        SetDst(inst.dst[0],
               ir.Imm32(DefaultValTable[attr.default_value][inst.control.vintrp.chan]));
        return;
    }
    ASSERT(!attr.is_flat);
    if (!profile.needs_manual_interpolation || !HasPerVertexInputs(profile)) {
        interp = GetInterpolation(vgpr_to_interp[inst.src[0].code]);
        SetDst(inst.dst[0], ir.GetAttribute(attrib, inst.control.vintrp.chan));
        return;
    }
    // VDST = P20 * VSRC + VDST
    const IR::F32 p0 = ir.GetAttribute(attrib, inst.control.vintrp.chan, 0);
    const IR::F32 p2 = ir.GetAttribute(attrib, inst.control.vintrp.chan, 2);
    const IR::F32 j = GetSrc<IR::F32>(inst.src[0]);
    const IR::F32 result = ir.FPFma(ir.FPSub(p2, p0), j, GetSrc<IR::F32>(inst.dst[0]));
    interp.primary = Qualifier::PerVertex;
    SetDst(inst.dst[0], result);
}

void Translator::V_INTERP_MOV_F32(const GcnInst& inst) {
    const u32 attr_index = inst.control.vintrp.attr;
    const IR::Attribute attrib = IR::Attribute::Param0 + attr_index;
    const auto& attr = runtime_info.hw.fs.inputs[attr_index];
    auto& interp = info.fs_interpolation[attr_index];
    const u32 src_select = inst.src[0].code;
    ASSERT_MSG(src_select < 3, "Invalid V_INTERP_MOV_F32 selector {}", src_select);
    if (HasPerVertexInputs(profile)) {
        // VSRC 0=P10, 1=P20, 2=P0
        interp.primary = Qualifier::PerVertex;
        IR::F32 result = ir.GetAttribute(attrib, inst.control.vintrp.chan, (src_select + 1) % 3);
        if (src_select != 2 && !attr.IsPassthrough()) {
            const IR::F32 p0 = ir.GetAttribute(attrib, inst.control.vintrp.chan, 0);
            result = ir.FPSub(result, p0);
        }
        SetDst(inst.dst[0], result);
    } else {
        // No per-vertex inputs (e.g. NVIDIA Pascal). Stand in for the primitive with a triangle
        // at this pixel: vertex 0 here, vertex 1 one pixel right, vertex 2 one pixel down, with
        // I = J = 0 (see EmitPrologue). P0 is then the interpolated value, P10/P20 are its
        // screen-space slopes, and P0 + I * P10 + J * P20 is what the hardware gives here.
        const IR::F32 value = ir.GetAttribute(attrib, inst.control.vintrp.chan);
        if (attr.is_flat) {
            // Every vertex holds the provoking vertex's value, so the slopes are 0.
            interp.primary = Qualifier::Flat;
        } else if (src_select == 2) {
            // P0 alone keeps the provoking vertex's value; an input that slopes or a
            // V_INTERP_P2 also read stays interpolated.
            if (interp.primary == Qualifier::None) {
                interp.primary = Qualifier::Flat;
            }
        } else if (interp.primary == Qualifier::None || interp.primary == Qualifier::Flat) {
            interp.primary = Qualifier::Smooth;
        }
        if (src_select == 2) {
            SetDst(inst.dst[0], value);
            return;
        }
        const auto [dx, dy] = QuadDeltas(ir, value);
        IR::F32 result = src_select == 0 ? dx : dy;
        if (attr.IsPassthrough()) {
            // Passthrough inputs (flat + OFFSET5) return the raw vertex value, not P1/P2 - P0.
            result = ir.FPAdd(value, result);
        }
        SetDst(inst.dst[0], result);
    }
}

} // namespace Shader::Gcn
