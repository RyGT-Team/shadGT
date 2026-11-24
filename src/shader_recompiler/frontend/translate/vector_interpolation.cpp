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

// PERF-SGT-151: on AMD's explicit vertex path (AMD-315, RX 6000 and older) a V_INTERP_MOV makes
// its whole attribute per-vertex, so P1/P2 reads of that attribute must be done by hand. Other
// attributes keep the hardware's interpolation: doing them all by hand cost 8% of a GT7 frame on
// a Radeon 610M (27 vertex reads in one pixel shader). The KHR barycentric path (NVIDIA, the
// single-sample barycentric fix) keeps every attribute by hand. SHADGT_MANUAL_INTERP_ALL=1: every
// attribute by hand on AMD too, as before (part of the shader store key).
void Translator::NoteInterpMovs(std::span<const GcnInst> program) {
    interp_mov_attrs = 0;
    for (const GcnInst& inst : program) {
        if (inst.opcode == Opcode::V_INTERP_MOV_F32 && inst.control.vintrp.attr < 64) {
            interp_mov_attrs |= u64{1} << inst.control.vintrp.attr;
        }
    }
}

bool Translator::ManualInterpolation(u32 attr_index) const {
    if (!profile.needs_manual_interpolation) {
        return false;
    }
    static const bool all = [] {
        const char* env = std::getenv("SHADGT_MANUAL_INTERP_ALL");
        return env && env[0] == '1';
    }();
    if (all || !profile.supports_amd_shader_explicit_vertex_parameter) {
        return true;
    }
    return attr_index < 64 && (interp_mov_attrs >> attr_index & 1) != 0;
}

// VINTRP

void Translator::V_INTERP_P1_F32(const GcnInst& inst) {
    const u32 attr_index = inst.control.vintrp.attr;
    if (!ManualInterpolation(attr_index)) {
        return;
    }
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
    if (!ManualInterpolation(attr_index)) {
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
    ASSERT(attr.is_flat || src_select == 2 ||
           profile.supports_amd_shader_explicit_vertex_parameter ||
           profile.supports_fragment_shader_barycentric);
    if (profile.supports_amd_shader_explicit_vertex_parameter ||
        profile.supports_fragment_shader_barycentric) {
        // VSRC 0=P10, 1=P20, 2=P0
        interp.primary = Qualifier::PerVertex;
        IR::F32 result = ir.GetAttribute(attrib, inst.control.vintrp.chan, (src_select + 1) % 3);
        if (src_select != 2 && !attr.IsPassthrough()) {
            const IR::F32 p0 = ir.GetAttribute(attrib, inst.control.vintrp.chan, 0);
            result = ir.FPSub(result, p0);
        }
        SetDst(inst.dst[0], result);
    } else {
        interp.primary = Qualifier::Flat;
        SetDst(inst.dst[0], ir.GetAttribute(attrib, inst.control.vintrp.chan));
    }
}

} // namespace Shader::Gcn
