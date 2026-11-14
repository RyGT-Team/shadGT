// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <span>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>
#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/func_traits.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

bool DirectLoadGroupsEnabled() {
    // SHADGT_DIRECT_LOAD_GROUPS=0 gives every read its own branch again (a separate store).
    static const bool enabled = [] {
        const char* env = std::getenv("SHADGT_DIRECT_LOAD_GROUPS");
        return !(env && env[0] == '0');
    }();
    return enabled;
}
namespace {

static constexpr spv::ExecutionMode GetInputPrimitiveType(AmdGpu::PrimitiveType type) {
    switch (type) {
    case AmdGpu::PrimitiveType::PointList:
        return spv::ExecutionMode::InputPoints;
    case AmdGpu::PrimitiveType::LineList:
    case AmdGpu::PrimitiveType::LineStrip:
        return spv::ExecutionMode::InputLines;
    case AmdGpu::PrimitiveType::TriangleList:
    case AmdGpu::PrimitiveType::TriangleStrip:
    case AmdGpu::PrimitiveType::RectList:
        return spv::ExecutionMode::Triangles;
    case AmdGpu::PrimitiveType::AdjTriangleList:
        return spv::ExecutionMode::InputTrianglesAdjacency;
    case AmdGpu::PrimitiveType::AdjLineList:
        return spv::ExecutionMode::InputLinesAdjacency;
    default:
        UNREACHABLE_MSG("Unknown input primitive type {}", u32(type));
    }
}

static constexpr spv::ExecutionMode GetOutputPrimitiveType(AmdGpu::GsOutputPrimitiveType type) {
    switch (type) {
    case AmdGpu::GsOutputPrimitiveType::PointList:
        return spv::ExecutionMode::OutputVertices;
    case AmdGpu::GsOutputPrimitiveType::LineStrip:
        return spv::ExecutionMode::OutputLineStrip;
    case AmdGpu::GsOutputPrimitiveType::TriangleStrip:
        return spv::ExecutionMode::OutputTriangleStrip;
    default:
        UNREACHABLE_MSG("Unknown output primitive type {}", u32(type));
    }
}

template <auto func, typename... Args>
void SetDefinition(EmitContext& ctx, IR::Inst* inst, Args... args) {
    inst->SetDefinition<Id>(func(ctx, std::forward<Args>(args)...));
}

template <typename ArgType>
ArgType Arg(EmitContext& ctx, const IR::Value& arg) {
    if constexpr (std::is_same_v<ArgType, Id>) {
        return ctx.Def(arg);
    } else if constexpr (std::is_same_v<ArgType, const IR::Value&>) {
        return arg;
    } else if constexpr (std::is_same_v<ArgType, u32>) {
        return arg.U32();
    } else if constexpr (std::is_same_v<ArgType, u64>) {
        return arg.U64();
    } else if constexpr (std::is_same_v<ArgType, bool>) {
        return arg.U1();
    } else if constexpr (std::is_same_v<ArgType, IR::Attribute>) {
        return arg.Attribute();
    } else if constexpr (std::is_same_v<ArgType, IR::ScalarReg>) {
        return arg.ScalarReg();
    } else if constexpr (std::is_same_v<ArgType, IR::VectorReg>) {
        return arg.VectorReg();
    } else if constexpr (std::is_same_v<ArgType, const char*>) {
        return arg.StringLiteral();
    } else if constexpr (std::is_same_v<ArgType, IR::Patch>) {
        return arg.Patch();
    }
    UNREACHABLE();
}

template <auto func, bool is_first_arg_inst, size_t... I>
void Invoke(EmitContext& ctx, IR::Inst* inst, std::index_sequence<I...>) {
    using Traits = Common::FuncTraits<decltype(func)>;
    if constexpr (std::is_same_v<typename Traits::ReturnType, Id>) {
        if constexpr (is_first_arg_inst) {
            SetDefinition<func>(
                ctx, inst, inst,
                Arg<typename Traits::template ArgType<I + 2>>(ctx, inst->Arg(I))...);
        } else {
            SetDefinition<func>(
                ctx, inst, Arg<typename Traits::template ArgType<I + 1>>(ctx, inst->Arg(I))...);
        }
    } else {
        if constexpr (is_first_arg_inst) {
            func(ctx, inst, Arg<typename Traits::template ArgType<I + 2>>(ctx, inst->Arg(I))...);
        } else {
            func(ctx, Arg<typename Traits::template ArgType<I + 1>>(ctx, inst->Arg(I))...);
        }
    }
}

template <auto func>
void Invoke(EmitContext& ctx, IR::Inst* inst) {
    using Traits = Common::FuncTraits<decltype(func)>;
    static_assert(Traits::NUM_ARGS >= 1, "Insufficient arguments");
    if constexpr (Traits::NUM_ARGS == 1) {
        Invoke<func, false>(ctx, inst, std::make_index_sequence<0>{});
    } else {
        using FirstArgType = typename Traits::template ArgType<1>;
        static constexpr bool is_first_arg_inst = std::is_same_v<FirstArgType, IR::Inst*>;
        using Indices = std::make_index_sequence<Traits::NUM_ARGS - (is_first_arg_inst ? 2 : 1)>;
        Invoke<func, is_first_arg_inst>(ctx, inst, Indices{});
    }
}

void EmitInst(EmitContext& ctx, IR::Inst* inst) {
    switch (inst->GetOpcode()) {
#define OPCODE(name, result_type, ...)                                                             \
    case IR::Opcode::name:                                                                         \
        return Invoke<&Emit##name>(ctx, inst);
#include "shader_recompiler/ir/opcodes.inc"
#undef OPCODE
    }
    UNREACHABLE_MSG("Invalid opcode {}", inst->GetOpcode());
}

Id TypeId(const EmitContext& ctx, IR::Type type) {
    switch (type) {
    case IR::Type::U1:
        return ctx.U1[1];
    case IR::Type::U32:
        return ctx.U32[1];
    case IR::Type::U32x2:
        return ctx.U32[2];
    case IR::Type::U32x3:
        return ctx.U32[3];
    case IR::Type::U32x4:
        return ctx.U32[4];
    case IR::Type::F32:
        return ctx.F32[1];
    case IR::Type::U64:
        return ctx.U64;
    default:
        UNREACHABLE_MSG("Phi node type {}", type);
    }
}

// PERF-GTS-473: instructions a buffer read is never moved past: side effects, GDS counters, and
// writes of registers or goto variables (a read of them must stay on its side of the write).
static bool StopsDirectGroup(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::Phi:
    case IR::Opcode::DataAppend:
    case IR::Opcode::DataConsume:
    case IR::Opcode::SetScalarRegister:
    case IR::Opcode::SetVectorRegister:
    case IR::Opcode::SetVirtualRegister:
    case IR::Opcode::SetGotoVariable:
    case IR::Opcode::SetScc:
    case IR::Opcode::SetExec:
    case IR::Opcode::SetVcc:
    case IR::Opcode::SetVccLo:
    case IR::Opcode::SetVccHi:
    case IR::Opcode::SetM0:
        return true;
    default:
        return inst.MayHaveSideEffects();
    }
}

// PERF-GTS-473: whether a value can be defined before the group's first read. Values from before
// the group (earlier in the block, other blocks, immediates) are; one defined in the block after
// the first read is if it has no side effects and its own arguments are (it is then emitted
// early, in `chain`).
static bool DefinedBeforeGroup(const IR::Value& value, const std::vector<IR::Inst*>& members,
                               const std::unordered_set<const IR::Inst*>& pending,
                               const std::unordered_set<const IR::Inst*>& hoisted,
                               std::vector<const IR::Inst*>& chain, u32 depth) {
    if (value.IsImmediate()) {
        return true;
    }
    const IR::Inst* inst = value.Inst();
    if (std::ranges::find(members, inst) != members.end()) {
        return false; // defined only after the group's branch
    }
    if (!pending.contains(inst) || hoisted.contains(inst) ||
        std::ranges::find(chain, inst) != chain.end()) {
        return true;
    }
    if (depth > 16 || StopsDirectGroup(*inst)) {
        return false;
    }
    for (size_t i = 0; i < inst->NumArgs(); ++i) {
        if (!DefinedBeforeGroup(inst->Arg(i), members, pending, hoisted, chain, depth + 1)) {
            return false;
        }
    }
    chain.push_back(inst);
    return true;
}

// PERF-GTS-473: a direct-eligible buffer read gets one direct/SSBO branch with the reads of the
// same buffer that follow it in the block, instead of a branch each: a 4x4 matrix read through
// four dwordx4 loads was 16 branches, each loading the table word and bounds again. Only reads
// are moved, and only past instructions without side effects (no stores, atomics, barriers or
// discards between), so every read returns what it did before. The address math a later read
// needs is emitted before the branch; everything else keeps its order after it.
static bool EmitDirectLoadGroup(EmitContext& ctx, IR::Block::iterator head,
                                IR::Block::iterator end, u32 handle,
                                std::unordered_set<const IR::Inst*>& emitted) {
    constexpr size_t max_scan = 96;
    constexpr size_t max_members = 32;
    std::vector<IR::Inst*> members{&*head};
    std::unordered_set<const IR::Inst*> pending;
    std::unordered_set<const IR::Inst*> hoisted;
    size_t scanned = 0;
    for (auto it = std::next(head); it != end && scanned < max_scan; ++it, ++scanned) {
        IR::Inst& inst = *it;
        if (emitted.contains(&inst)) {
            continue; // emitted early by an earlier group
        }
        if (StopsDirectGroup(inst)) {
            break;
        }
        if (GroupableDirectLoad(ctx, inst) == handle) {
            std::vector<const IR::Inst*> chain;
            bool ready = true;
            for (size_t i = 0; i < inst.NumArgs() && ready; ++i) {
                ready = DefinedBeforeGroup(inst.Arg(i), members, pending, hoisted, chain, 0);
            }
            if (ready) {
                hoisted.insert(chain.begin(), chain.end());
                members.push_back(&inst);
                if (members.size() == max_members) {
                    break;
                }
                continue;
            }
        }
        pending.insert(&inst);
    }
    if (members.size() < 2) {
        return false;
    }
    // Address math of the later reads, in block order.
    for (auto it = std::next(head); &*it != members.back(); ++it) {
        if (hoisted.contains(&*it)) {
            EmitInst(ctx, &*it);
            emitted.insert(&*it);
        }
    }
    std::vector<DirectLoadDesc> descs;
    std::vector<EmitContext::DirectPart> parts;
    descs.reserve(members.size());
    for (IR::Inst* member : members) {
        descs.push_back(DescribeDirectLoad(ctx, member));
        parts.insert(parts.end(), descs.back().parts.begin(), descs.back().parts.end());
    }
    const std::vector<Id> values = ctx.EmitDirectLoads(handle, parts);
    size_t first = 0;
    for (size_t i = 0; i < members.size(); ++i) {
        const size_t count = descs[i].parts.size();
        members[i]->SetDefinition<Id>(
            descs[i].combine(std::span<const Id>(values).subspan(first, count)));
        first += count;
        emitted.insert(members[i]);
    }
    return true;
}

void Traverse(EmitContext& ctx, const IR::Program& program) {
    IR::Block* current_block{};
    std::unordered_set<const IR::Inst*> emitted; // PERF-GTS-473: emitted ahead of their place
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Block: {
            const Id label{node.data.block->Definition<Id>()};
            if (current_block) {
                ctx.OpBranch(label);
            }
            current_block = node.data.block;
            ctx.AddLabel(label);
            auto& insts = node.data.block->Instructions();
            emitted.clear();
            for (auto it = insts.begin(); it != insts.end(); ++it) {
                if (!emitted.empty() && emitted.contains(&*it)) {
                    continue;
                }
                if (const auto handle = GroupableDirectLoad(ctx, *it);
                    handle && EmitDirectLoadGroup(ctx, it, insts.end(), *handle, emitted)) {
                    continue;
                }
                EmitInst(ctx, &*it);
            }
            ctx.first_to_last_label_map[label.value] = ctx.last_label;
            break;
        }
        case IR::AbstractSyntaxNode::Type::If: {
            const Id if_label{node.data.if_node.body->Definition<Id>()};
            const Id endif_label{node.data.if_node.merge->Definition<Id>()};
            ctx.OpSelectionMerge(endif_label, spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(ctx.Def(node.data.if_node.cond), if_label, endif_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::Loop: {
            const Id body_label{node.data.loop.body->Definition<Id>()};
            const Id continue_label{node.data.loop.continue_block->Definition<Id>()};
            const Id endloop_label{node.data.loop.merge->Definition<Id>()};

            ctx.OpLoopMerge(endloop_label, continue_label, spv::LoopControlMask::MaskNone);
            ctx.OpBranch(body_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::Break: {
            const Id break_label{node.data.break_node.merge->Definition<Id>()};
            const Id skip_label{node.data.break_node.skip->Definition<Id>()};
            ctx.OpBranchConditional(ctx.Def(node.data.break_node.cond), break_label, skip_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::EndIf:
            if (current_block) {
                ctx.OpBranch(node.data.end_if.merge->Definition<Id>());
            }
            break;
        case IR::AbstractSyntaxNode::Type::Repeat: {
            Id cond{ctx.Def(node.data.repeat.cond)};
            const Id loop_header_label{node.data.repeat.loop_header->Definition<Id>()};
            const Id merge_label{node.data.repeat.merge->Definition<Id>()};
            ctx.OpBranchConditional(cond, loop_header_label, merge_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::Return:
            ctx.OpReturn();
            break;
        case IR::AbstractSyntaxNode::Type::Unreachable:
            ctx.OpUnreachable();
            break;
        }
        if (node.type != IR::AbstractSyntaxNode::Type::Block) {
            current_block = nullptr;
        }
    }
}

Id DefineMain(EmitContext& ctx, const IR::Program& program) {
    const Id void_function{ctx.TypeFunction(ctx.void_id)};
    const Id main{ctx.OpFunction(ctx.void_id, spv::FunctionControlMask::MaskNone, void_function)};
    for (IR::Block* const block : program.blocks) {
        block->SetDefinition(ctx.OpLabel());
    }
    Traverse(ctx, program);
    ctx.OpFunctionEnd();
    return main;
}

spv::ExecutionMode ExecutionMode(AmdGpu::TessellationType primitive) {
    switch (primitive) {
    case AmdGpu::TessellationType::Isoline:
        return spv::ExecutionMode::Isolines;
    case AmdGpu::TessellationType::Triangle:
        return spv::ExecutionMode::Triangles;
    case AmdGpu::TessellationType::Quad:
        return spv::ExecutionMode::Quads;
    }
    UNREACHABLE_MSG("Tessellation primitive {}", magic_enum::enum_name(primitive));
}

spv::ExecutionMode ExecutionMode(AmdGpu::TessellationPartitioning spacing) {
    switch (spacing) {
    case AmdGpu::TessellationPartitioning::Integer:
        return spv::ExecutionMode::SpacingEqual;
    case AmdGpu::TessellationPartitioning::FracOdd:
        return spv::ExecutionMode::SpacingFractionalOdd;
    case AmdGpu::TessellationPartitioning::FracEven:
        return spv::ExecutionMode::SpacingFractionalEven;
    case AmdGpu::TessellationPartitioning::Pow2:
        // Pow2 rounds tessellation factors to the nearest power of 2, which has no
        // direct Vulkan equivalent. SpacingEqual (integer) is the closest match.
        LOG_WARNING(Render_Vulkan, "Tessellation partitioning Pow2 has no Vulkan equivalent, "
                                   "falling back to SpacingEqual");
        return spv::ExecutionMode::SpacingEqual;
    default:
        break;
    }
    UNREACHABLE_MSG("Tessellation spacing {}", magic_enum::enum_name(spacing));
}

void SetupCapabilities(const Info& info, const Profile& profile, const RuntimeInfo& runtime_info,
                       EmitContext& ctx) {
    ctx.AddCapability(spv::Capability::Image1D);
    ctx.AddCapability(spv::Capability::Sampled1D);
    ctx.AddCapability(spv::Capability::ImageQuery);
    ctx.AddCapability(spv::Capability::Int8);
    ctx.AddCapability(spv::Capability::Int16);
    ctx.AddCapability(spv::Capability::Int64);
    ctx.AddCapability(spv::Capability::StorageBuffer8BitAccess);
    ctx.AddCapability(spv::Capability::StorageBuffer16BitAccess);
    if (info.uses_fp16) {
        ctx.AddCapability(spv::Capability::Float16);
    }
    if (info.uses_fp64) {
        ctx.AddCapability(spv::Capability::Float64);
    }
    if (info.has_storage_images) {
        ctx.AddCapability(spv::Capability::StorageImageExtendedFormats);
        ctx.AddCapability(spv::Capability::StorageImageReadWithoutFormat);
        ctx.AddCapability(spv::Capability::StorageImageWriteWithoutFormat);
        if (profile.supports_image_load_store_lod) {
            ctx.AddExtension("SPV_AMD_shader_image_load_store_lod");
            ctx.AddCapability(spv::Capability::ImageReadWriteLodAMD);
        }
    }
    if (info.has_image_gather) {
        ctx.AddCapability(spv::Capability::ImageGatherExtended);
    }
    if (info.has_image_query) {
        ctx.AddCapability(spv::Capability::ImageQuery);
    }
    if ((info.uses_image_atomic_float_min_max && profile.supports_image_fp32_atomic_min_max) ||
        (info.uses_buffer_atomic_float_min_max && profile.supports_buffer_fp32_atomic_min_max)) {
        ctx.AddExtension("SPV_EXT_shader_atomic_float_min_max");
        ctx.AddCapability(spv::Capability::AtomicFloat32MinMaxEXT);
    }
    if (info.uses_lane_id) {
        ctx.AddCapability(spv::Capability::GroupNonUniform);
    }
    if (info.uses_group_quad) {
        ctx.AddCapability(spv::Capability::GroupNonUniformQuad);
    }
    if (info.uses_group_shuffle) {
        ctx.AddCapability(spv::Capability::GroupNonUniformShuffle);
    }
    if (info.uses_group_ballot || info.loads.Get(IR::Attribute::SubgroupLtMask)) {
        ctx.AddCapability(spv::Capability::GroupNonUniformBallot);
    }
    if (info.uses_shader_clock) {
        if (ctx.profile.supports_shader_subgroup_clock) {
            ctx.AddExtension("SPV_KHR_shader_clock");
            ctx.AddCapability(spv::Capability::ShaderClockKHR);
        } else {
            LOG_WARNING(Render_Recompiler,
                        "Shader requires support for ShaderClockKHR capability"
                        " that your Vulkan instance does not advertise. Results may vary");
        }
    }
    const auto stage = info.sw_stage;
    if (stage == SwStage::Vertex) {
        ctx.AddExtension("SPV_KHR_shader_draw_parameters");
        ctx.AddCapability(spv::Capability::DrawParameters);
    }
    if (stage == SwStage::Geometry) {
        ctx.AddCapability(spv::Capability::Geometry);
    }
    if (info.hw_stage == HwStage::Fragment) {
        if (profile.supports_amd_shader_explicit_vertex_parameter) {
            ctx.AddExtension("SPV_AMD_shader_explicit_vertex_parameter");
        } else if (profile.supports_fragment_shader_barycentric) {
            ctx.AddExtension("SPV_KHR_fragment_shader_barycentric");
            ctx.AddCapability(spv::Capability::FragmentBarycentricKHR);
            ctx.AddCapability(spv::Capability::InterpolationFunction);
        }
        if (info.loads.Get(IR::Attribute::SampleIndex) ||
            runtime_info.hw.fs.addr_flags.linear_sample_ena ||
            runtime_info.hw.fs.addr_flags.persp_sample_ena ||
            (!profile.supports_amd_shader_explicit_vertex_parameter &&
             profile.supports_fragment_shader_barycentric &&
             info.loads.Get(IR::Attribute::BaryCoordSmoothSample))) {
            ctx.AddCapability(spv::Capability::SampleRateShading);
        }
        if (info.loads.GetAny(IR::Attribute::RenderTargetIndex)) {
            ctx.AddCapability(spv::Capability::Geometry);
        }
        if (info.stores.Get(IR::Attribute::StencilRef) && profile.supports_shader_stencil_export) {
            ctx.AddExtension("SPV_EXT_shader_stencil_export");
            ctx.AddCapability(spv::Capability::StencilExportEXT);
        }
    }
    if (stage == SwStage::TessellationControl || stage == SwStage::TessellationEval) {
        ctx.AddCapability(spv::Capability::Tessellation);
    }
    if (stage == SwStage::Vertex || stage == SwStage::TessellationControl ||
        stage == SwStage::TessellationEval) {
        if (info.stores.GetAny(IR::Attribute::RenderTargetIndex)) {
            ctx.AddCapability(spv::Capability::ShaderLayer);
        }
        if (info.stores.GetAny(IR::Attribute::ViewportIndex)) {
            ctx.AddCapability(spv::Capability::ShaderViewportIndex);
        }
    } else if (stage == SwStage::Geometry && info.stores.GetAny(IR::Attribute::ViewportIndex)) {
        ctx.AddCapability(spv::Capability::MultiViewport);
    }
    if (info.uses_dma || info.uses_direct ||
        (stage == SwStage::TessellationControl && runtime_info.sw.tcs.lds_emulation != 0)) { // FIX-227
        ctx.AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
        ctx.AddExtension("SPV_KHR_physical_storage_buffer");
    }
    const auto shared_type_count = std::popcount(static_cast<u32>(info.shared_types));
    if (shared_type_count > 1 && profile.supports_workgroup_explicit_memory_layout) {
        ctx.AddExtension("SPV_KHR_workgroup_memory_explicit_layout");
        ctx.AddCapability(spv::Capability::WorkgroupMemoryExplicitLayoutKHR);
        ctx.AddCapability(spv::Capability::WorkgroupMemoryExplicitLayout16BitAccessKHR);
    }
    if (info.uses_buffer_int64_atomics || info.uses_shared_int64_atomics) {
        if (info.uses_buffer_int64_atomics) {
            ASSERT_MSG(ctx.profile.supports_buffer_int64_atomics,
                       "Shader requires support for atomic Int64 buffer operations that your "
                       "Vulkan instance does not advertise");
        }
        if (info.uses_shared_int64_atomics) {
            ASSERT_MSG(ctx.profile.supports_shared_int64_atomics,
                       "Shader requires support for atomic Int64 shared memory operations that "
                       "your Vulkan instance does not advertise");
        }
        ctx.AddCapability(spv::Capability::Int64Atomics);
    }
}

void DefineEntryPoint(const Info& info, EmitContext& ctx, Id main) {
    const std::span interfaces(ctx.interfaces.data(), ctx.interfaces.size());
    spv::ExecutionModel execution_model{};
    switch (info.sw_stage) {
    case SwStage::Compute: {
        const std::array<u32, 3> workgroup_size{ctx.runtime_info.hw.cs.workgroup_size};
        execution_model = spv::ExecutionModel::GLCompute;
        ctx.AddExecutionMode(main, spv::ExecutionMode::LocalSize, workgroup_size[0],
                             workgroup_size[1], workgroup_size[2]);
        break;
    }
    case SwStage::Vertex:
        execution_model = spv::ExecutionModel::Vertex;
        break;
    case SwStage::TessellationControl:
        execution_model = spv::ExecutionModel::TessellationControl;
        ctx.AddCapability(spv::Capability::Tessellation);
        ctx.AddExecutionMode(main, spv::ExecutionMode::OutputVertices,
                             ctx.runtime_info.sw.tcs.NumOutputControlPoints());
        break;
    case SwStage::TessellationEval: {
        execution_model = spv::ExecutionModel::TessellationEvaluation;
        ctx.AddExecutionMode(main, ExecutionMode(ctx.runtime_info.sw.tes.tess_type));
        ctx.AddExecutionMode(main, ExecutionMode(ctx.runtime_info.sw.tes.tess_partitioning));
        ctx.AddExecutionMode(main, ctx.runtime_info.sw.tes.tess_topology ==
                                           AmdGpu::TessellationTopology::TriangleCcw
                                       ? spv::ExecutionMode::VertexOrderCcw
                                       : spv::ExecutionMode::VertexOrderCw);
        break;
    }
    case SwStage::Fragment:
        execution_model = spv::ExecutionModel::Fragment;
        if (ctx.profile.lower_left_origin_mode) {
            ctx.AddExecutionMode(main, spv::ExecutionMode::OriginLowerLeft);
        } else {
            ctx.AddExecutionMode(main, spv::ExecutionMode::OriginUpperLeft);
        }
        if (info.has_discard) {
            ctx.AddCapability(spv::Capability::DemoteToHelperInvocation);
        }
        // AMD-306: GCN computes a texture's mip level from all four pixels of a quad, also inside
        // branches. In SPIR-V implicit derivatives in a branch some quad pixels skip are
        // undefined; AMD's RX 6000 (and older) compiler then picked wrong mip levels and smeared
        // wall and kerb textures (RX 6600 XT), where the RX 7000 compiler kept the quads whole.
        if (ctx.profile.supports_quad_derivatives) {
            ctx.AddExtension("SPV_KHR_maximal_reconvergence");
            ctx.AddExtension("SPV_KHR_quad_control");
            ctx.AddCapability(spv::Capability::QuadControlKHR);
            ctx.AddExecutionMode(main, spv::ExecutionMode::MaximallyReconvergesKHR);
            ctx.AddExecutionMode(main, spv::ExecutionMode::QuadDerivativesKHR);
            ctx.AddExecutionMode(main, spv::ExecutionMode::RequireFullQuadsKHR);
        }
        if (info.stores.GetAny(IR::Attribute::Depth)) {
            ctx.AddExecutionMode(main, spv::ExecutionMode::DepthReplacing);
        }
        break;
    case SwStage::Geometry:
        execution_model = spv::ExecutionModel::Geometry;
        ctx.AddExecutionMode(main, GetInputPrimitiveType(ctx.runtime_info.hw.gs.in_primitive));
        ctx.AddExecutionMode(main, GetOutputPrimitiveType(ctx.runtime_info.hw.gs.out_primitive[0]));
        ctx.AddExecutionMode(main, spv::ExecutionMode::OutputVertices,
                             ctx.runtime_info.hw.gs.output_vertices);
        ctx.AddExecutionMode(main, spv::ExecutionMode::Invocations,
                             ctx.runtime_info.hw.gs.num_invocations);
        break;
    default:
        UNREACHABLE_MSG("Stage {}", u32(info.hw_stage));
    }
    ctx.AddEntryPoint(execution_model, main, "main", interfaces);
}

void SetupDenormFlushMode(EmitContext& ctx, const Profile& profile, const RuntimeInfo& runtime_info,
                          Id main_func) {
    const auto fp32_denorm_mode = runtime_info.props.fp_denorm_mode32;
    if (fp32_denorm_mode == AmdGpu::FpDenormMode::InOutFlush) {
        if (profile.support_fp32_denorm_flush) {
            ctx.AddCapability(spv::Capability::DenormFlushToZero);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormFlushToZero, 32U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float32 denorm flushing is not supported by the GPU");
            });
        }
    } else {
        LOG_WARNING(Render_Vulkan, "Unknown FP denorm mode {}", u32(fp32_denorm_mode));
    }

    if (!ctx.info.uses_fp16 && !ctx.info.uses_fp64) {
        return;
    }

    const auto fp16_64_denorm_mode = runtime_info.props.fp_denorm_mode16_64;
    const auto same_denorm_mode = fp16_64_denorm_mode == fp32_denorm_mode;
    if (!same_denorm_mode && !profile.supports_denorm_behavior_independence) {
        static std::once_flag logged;
        std::call_once(logged, [] {
            LOG_WARNING(Render_Vulkan, "Float32 and Float16/64 denorm modes are different but the "
                                       "GPU does not support that");
        });
        return;
    }

    if (fp16_64_denorm_mode == AmdGpu::FpDenormMode::InOutFlush) {
        if (profile.support_fp16_denorm_flush) {
            ctx.AddCapability(spv::Capability::DenormFlushToZero);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormFlushToZero, 16U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float16 denorm flushing is not supported by the GPU");
            });
        }
        if (profile.support_fp64_denorm_flush) {
            ctx.AddCapability(spv::Capability::DenormFlushToZero);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormFlushToZero, 64U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float64 denorm flushing is not supported by the GPU");
            });
        }
    } else if (fp16_64_denorm_mode == AmdGpu::FpDenormMode::InOutAllow) {
        if (profile.support_fp16_denorm_preserve) {
            ctx.AddCapability(spv::Capability::DenormPreserve);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormPreserve, 16U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float16 denorm preserving is not supported by the GPU");
            });
        }
        if (profile.support_fp64_denorm_preserve) {
            ctx.AddCapability(spv::Capability::DenormPreserve);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormPreserve, 64U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float64 denorm preserving is not supported by the GPU");
            });
        }
    } else {
        LOG_WARNING(Render_Vulkan, "Unknown Float16/64 denorm mode {}", u32(fp16_64_denorm_mode));
    }
}

void SetupRoundingMode(EmitContext& ctx, const Profile& profile, const RuntimeInfo& runtime_info,
                       Id main_func) {
    const auto fp_round_mode = runtime_info.props.fp_round_mode32;
    if (fp_round_mode == AmdGpu::FpRoundMode::ToZero) {
        if (profile.support_fp32_round_to_zero) {
            ctx.AddCapability(spv::Capability::RoundingModeRTZ);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::RoundingModeRTZ, 32U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float32 rounding to zero is not supported by the GPU");
            });
        }
    } else if (fp_round_mode != AmdGpu::FpRoundMode::NearestEven) {
        LOG_WARNING(Render_Vulkan, "Unimplemented FP rounding mode {}",
                    magic_enum::enum_name(fp_round_mode));
    }

    if (ctx.info.uses_fp16 || ctx.info.uses_fp64) {
        const auto fp16_64_round_mode = runtime_info.props.fp_round_mode16_64;
        const auto same_round_mode = fp16_64_round_mode == fp_round_mode;
        if (!same_round_mode && !profile.supports_rounding_mode_independence) {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float32 and Float16/64 rounding modes are different "
                                           "but the GPU does not support that");
            });
            return;
        }

        if (fp16_64_round_mode == AmdGpu::FpRoundMode::ToZero) {
            if (ctx.info.uses_fp16) {
                if (profile.support_fp16_round_to_zero) {
                    ctx.AddCapability(spv::Capability::RoundingModeRTZ);
                    ctx.AddExecutionMode(main_func, spv::ExecutionMode::RoundingModeRTZ, 16U);
                } else {
                    static std::once_flag logged;
                    std::call_once(logged, [] {
                        LOG_WARNING(Render_Vulkan,
                                    "Float16 rounding to zero is not supported by the GPU");
                    });
                }
            }
            if (ctx.info.uses_fp64) {
                if (profile.support_fp64_round_to_zero) {
                    ctx.AddCapability(spv::Capability::RoundingModeRTZ);
                    ctx.AddExecutionMode(main_func, spv::ExecutionMode::RoundingModeRTZ, 64U);
                } else {
                    static std::once_flag logged;
                    std::call_once(logged, [] {
                        LOG_WARNING(Render_Vulkan,
                                    "Float64 rounding to zero is not supported by the GPU");
                    });
                }
            }
        } else if (fp16_64_round_mode != AmdGpu::FpRoundMode::NearestEven) {
            LOG_WARNING(Render_Vulkan, "Unknown Float16/64 rounding mode {}",
                        u32(fp16_64_round_mode));
        }
    }
}

void SetupInfNanPreserveMode(EmitContext& ctx, const Profile& profile,
                             const RuntimeInfo& runtime_info, Id main_func) {
    if (profile.support_fp16_signed_zero_inf_nan_preserve ||
        profile.support_fp32_signed_zero_inf_nan_preserve ||
        profile.support_fp64_signed_zero_inf_nan_preserve) {
        ctx.AddCapability(spv::Capability::SignedZeroInfNanPreserve);
    }

    if (profile.support_fp32_signed_zero_inf_nan_preserve) {
        ctx.AddExecutionMode(main_func, spv::ExecutionMode::SignedZeroInfNanPreserve, 32U);
    } else {
        static std::once_flag logged;
        std::call_once(logged, [] {
            LOG_WARNING(Render_Vulkan,
                        "Float32 signed zero/inf/nan preserve mode is not supported by the GPU");
        });
    }
    if (ctx.info.uses_fp16) {
        if (profile.support_fp16_signed_zero_inf_nan_preserve) {
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::SignedZeroInfNanPreserve, 16U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(
                    Render_Vulkan,
                    "Float16 signed zero/inf/nan preserve mode is not supported by the GPU");
            });
        }
    }
    if (ctx.info.uses_fp64) {
        if (profile.support_fp64_signed_zero_inf_nan_preserve) {
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::SignedZeroInfNanPreserve, 64U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(
                    Render_Vulkan,
                    "Float64 signed zero/inf/nan preserve mode is not supported by the GPU");
            });
        }
    }
}

void SetupFloatMode(EmitContext& ctx, const Profile& profile, const RuntimeInfo& runtime_info,
                    Id main_func) {
    ctx.AddExtension("SPV_KHR_float_controls");
    SetupDenormFlushMode(ctx, profile, runtime_info, main_func);
    SetupRoundingMode(ctx, profile, runtime_info, main_func);
    SetupInfNanPreserveMode(ctx, profile, runtime_info, main_func);
}

void PatchPhiNodes(const IR::Program& program, EmitContext& ctx) {
    auto inst{program.blocks.front()->begin()};
    size_t block_index{0};
    ctx.PatchDeferredPhi([&](u32 phi_arg, Id first_parent) {
        if (phi_arg == 0) {
            ++inst;
            while (inst == program.blocks[block_index]->end() ||
                   inst->GetOpcode() != IR::Opcode::Phi) {
                ++block_index;
                inst = program.blocks[block_index]->begin();
            }
        }
        ASSERT(inst != program.blocks[block_index]->end());
        const Id arg = ctx.Def(inst->Arg(phi_arg));
        const Id parent = ctx.first_to_last_label_map[first_parent.value];
        return std::make_pair(arg, parent);
    });
}
} // Anonymous namespace

std::vector<u32> EmitSPIRV(const Profile& device_profile, const RuntimeInfo& runtime_info,
                           const IR::Program& program, Bindings& binding) {
    auto profile = device_profile;
    // AMD explicit interpolation and KHR per-vertex arrays are alternative ABIs.
    // Both extensions may be enabled on the device, but a shader must select one.
    if (profile.supports_amd_shader_explicit_vertex_parameter) {
        profile.supports_fragment_shader_barycentric = false;
    }
    if (runtime_info.hw_stage == HwStage::Fragment &&
        runtime_info.hw.fs.single_sample_barycentric) {
        profile.supports_amd_shader_explicit_vertex_parameter = false;
        profile.supports_fragment_shader_barycentric = true;
        // Preserve P1/P2 arithmetic when MOV makes another component per-vertex.
        // Otherwise the shared attribute qualifier turns prior hardware reads into P0.
        profile.needs_manual_interpolation = true;
    }
    EmitContext ctx{profile, runtime_info, program.info, binding};
    const Id main{DefineMain(ctx, program)};
    DefineEntryPoint(program.info, ctx, main);
    SetupCapabilities(program.info, profile, runtime_info, ctx);
    SetupFloatMode(ctx, profile, runtime_info, main);
    PatchPhiNodes(program, ctx);
    return ctx.Assemble();
}

Id EmitPhi(EmitContext& ctx, IR::Inst* inst) {
    const size_t num_args{inst->NumArgs()};
    boost::container::small_vector<Id, 32> blocks;
    blocks.reserve(num_args);
    for (size_t index = 0; index < num_args; ++index) {
        blocks.push_back(inst->PhiBlock(index)->Definition<Id>());
    }
    // The type of a phi instruction is stored in its flags
    const Id result_type{TypeId(ctx, inst->Flags<IR::Type>())};
    return ctx.DeferredOpPhi(result_type, std::span(blocks.data(), blocks.size()));
}

void EmitVoid(EmitContext&) {}

Id EmitConditionRef(EmitContext& ctx, const IR::Value& value) {
    const Id id{ctx.Def(value)};
    ASSERT_MSG(Sirit::ValidId(id), "Forward identity declaration");
    return id;
}

Id EmitGetPcLo(EmitContext& ctx, const IR::Value& value) {
    const Id id{ctx.Def(value)};
    ASSERT_MSG(Sirit::ValidId(id), "Forward identity declaration");
    return id;
}

void EmitGetScc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetExec(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVcc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVccLo(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVccHi(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetM0(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetScc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetExec(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVcc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVccLo(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVccHi(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetM0(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

} // namespace Shader::Backend::SPIRV
