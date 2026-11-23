// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// FIX-SGT-115: texture and sampler descriptors that a shader reads only in a branch that is off.
// The SRT walker copies them at every draw whatever the branch, so the translation, its key and
// the bindings were made from what an unused slot was left holding: GT7's depth-to-distance pass
// (ps 0x2a265dff) samples two textures only if a word of the same table is not zero, the game
// leaves floats in their slots while it is, and each launch translated new variants of it.

#include <algorithm>
#include <cstdlib>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/types.h"
#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/params.h"
#include "shader_recompiler/resource.h"

namespace Shader::Optimization {

/// SHADGT_BRANCH_OFF_DESCRIPTORS=0: descriptors are copied whatever their branch, as before. The
/// shader store then has a version of its own (cache_storage.cpp): the walkers are stored.
inline bool BranchOffDescriptorsEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADGT_BRANCH_OFF_DESCRIPTORS");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

/// Descriptor words that nothing reads unless a branch on a word of the same flattened buffer is
/// taken. That word is the one the shader itself branches on, from the walker's copy; when the
/// binding may copy it again from what the GPU wrote, the walk keeps the descriptors
/// (Info::RefreshFlatBuf). The generated walker empties them, and so does the translation of a
/// read log's replay (FlattenExtendedUserdataPass), never a shader record's.
struct DescriptorGuard {
    u16 word{};
    u32 value{};
    bool taken_if_equal{};
    std::vector<u16> descriptor_words;

    bool Off(std::span<const u32> flat) const {
        return word < flat.size() && (flat[word] == value) != taken_if_equal;
    }

    /// What the generated walker does, for a flattened buffer filled without it.
    void Apply(std::span<u32> flat, std::span<u64> sources) const {
        if (!Off(flat)) {
            return;
        }
        for (const u16 at : descriptor_words) {
            if (at < flat.size()) {
                flat[at] = 0;
            }
            if (at < sources.size()) {
                sources[at] = 0;
            }
        }
    }
};

namespace GuardDetail {

using BlockSet = std::unordered_set<const IR::Block*>;

/// Flattened buffer word of a read, or zero. Words below the user data registers are never one.
inline u16 FlatWord(const IR::Inst* inst) {
    if (!inst) {
        return 0;
    }
    u16 word = 0;
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        word = inst->Flags<u16>();
    } else if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        word = inst->Flags<IR::BufferInstInfo>().flatbuf_off_dw;
    }
    return word >= NUM_USER_DATA_REGS ? word : u16{0};
}

struct Condition {
    u16 word{};
    u32 value{};
    bool true_if_equal{};
};

/// A branch condition that is only a flattened word compared with a constant. The shader reads a
/// ReadConst from the flattened copy; a ReadConstBuffer it reads from the buffer, so not that one.
inline bool MatchCondition(IR::Value value, Condition& out) {
    bool negated = false;
    for (u32 depth = 0; depth < 16; ++depth) {
        if (value.IsEmpty() || value.IsImmediate()) {
            return false;
        }
        const IR::Inst* inst = value.Inst();
        switch (inst->GetOpcode()) {
        case IR::Opcode::ConditionRef:
            value = inst->Arg(0);
            continue;
        case IR::Opcode::LogicalNot:
            negated = !negated;
            value = inst->Arg(0);
            continue;
        case IR::Opcode::IEqual32:
        case IR::Opcode::INotEqual32: {
            IR::Value read = inst->Arg(0);
            IR::Value constant = inst->Arg(1);
            if (read.IsImmediate()) {
                std::swap(read, constant);
            }
            if (read.IsImmediate() || !constant.IsImmediate()) {
                return false;
            }
            const IR::Inst* source = read.Inst();
            if (source->GetOpcode() != IR::Opcode::ReadConst || FlatWord(source) == 0) {
                return false;
            }
            out.word = FlatWord(source);
            out.value = constant.U32();
            out.true_if_equal = (inst->GetOpcode() == IR::Opcode::IEqual32) != negated;
            return true;
        }
        default:
            return false;
        }
    }
    return false;
}

inline BlockSet Reachable(const IR::Block* entry, const IR::Block* skip_from = nullptr,
                          const IR::Block* skip_to = nullptr) {
    BlockSet seen{entry};
    std::vector<const IR::Block*> pending{entry};
    while (!pending.empty()) {
        const IR::Block* block = pending.back();
        pending.pop_back();
        for (const IR::Block* next : block->ImmSuccessors()) {
            if (block == skip_from && next == skip_to) {
                continue;
            }
            if (seen.insert(next).second) {
                pending.push_back(next);
            }
        }
    }
    return seen;
}

/// The instructions whose descriptors may be emptied: reads of sampled images. A descriptor the
/// translation also computes with (a post-op) keeps its words.
inline std::unordered_set<const IR::Inst*> DescriptorUsers(
    const ResourceDiscoveryList& resources) {
    std::unordered_set<const IR::Inst*> users;
    for (const auto& resource : resources) {
        if (!resource.user || resource.sharps[0].post_op != SharpFetchPostOp{} ||
            resource.sharps[1].post_op != SharpFetchPostOp{}) {
            continue;
        }
        switch (resource.user->GetOpcode()) {
        case IR::Opcode::ImageSampleRaw:
        case IR::Opcode::ImageRead:
        case IR::Opcode::ImageQueryDimensions:
        case IR::Opcode::ImageQueryLod:
            users.insert(resource.user);
            break;
        default:
            break;
        }
    }
    return users;
}

/// Whether everything that can observe `inst` is a descriptor operand under the branch.
inline bool OnlyDescriptorUses(const IR::Inst* inst, const BlockSet& under,
                               const std::unordered_set<const IR::Inst*>& users, u32 depth = 0) {
    if (depth > 8) {
        return false;
    }
    for (const auto& use : inst->Uses()) {
        const IR::Inst* user = use.user;
        switch (user->GetOpcode()) {
        case IR::Opcode::CompositeConstructU32x2:
        case IR::Opcode::CompositeConstructU32x3:
        case IR::Opcode::CompositeConstructU32x4:
        case IR::Opcode::ImageHandle:
            if (!OnlyDescriptorUses(user, under, users, depth + 1)) {
                return false;
            }
            break;
        case IR::Opcode::SetScalarRegister:
            // The translator writes every loaded word to its register. SSA has run, so whatever
            // reads the register has the word itself or a phi of it among its arguments, and
            // LowerPhisToRegsPass only makes register reads of those phis: each is a use seen here.
            if (!under.contains(user->GetParent())) {
                return false;
            }
            break;
        default: {
            const bool image = use.operand == 0;
            const bool sampler =
                use.operand == 1 && user->GetOpcode() == IR::Opcode::ImageSampleRaw;
            if (!(image || sampler) || !users.contains(user) || !under.contains(user->GetParent())) {
                return false;
            }
        }
        }
    }
    return true;
}

} // namespace GuardDetail

/// Runs on the unstructured graph, once every read has its flattened word (duplicates too).
/// `reads`: all of the program's flattened reads.
inline std::vector<DescriptorGuard> FindDescriptorGuards(const IR::Program& program,
                                                         const ResourceDiscoveryList& resources,
                                                         std::span<IR::Inst* const> reads) {
    using namespace GuardDetail;
    std::vector<DescriptorGuard> guards;
    const auto stage = program.info.sw_stage;
    // The tessellation transforms rewrite the graph the branches are taken from.
    if (program.blocks.empty() || resources.empty() || stage == SwStage::TessellationControl ||
        stage == SwStage::TessellationEval) {
        return guards;
    }
    const auto users = DescriptorUsers(resources);
    if (users.empty()) {
        return guards;
    }
    std::unordered_map<u16, std::vector<const IR::Inst*>> readers;
    for (const IR::Inst* read : reads) {
        if (const u16 word = FlatWord(read)) {
            readers[word].push_back(read);
        }
    }
    const IR::Block* entry = program.blocks.front();
    BlockSet reachable;

    for (const IR::Block* block : program.blocks) {
        const Gcn::Block* cfg = block->cfg_block;
        if (!cfg || !cfg->branch_true || !cfg->branch_false) {
            continue;
        }
        const IR::Block* taken = cfg->branch_true->ir_block;
        const IR::Block* other = cfg->branch_false->ir_block;
        Condition condition;
        if (!taken || !other || taken == other ||
            !MatchCondition(IR::Value{block->branch_cond}, condition)) {
            continue;
        }
        if (reachable.empty()) {
            reachable = Reachable(entry);
        }
        for (const bool true_edge : {true, false}) {
            const auto without = Reachable(entry, block, true_edge ? taken : other);
            BlockSet under;
            for (const IR::Block* b : reachable) {
                if (!without.contains(b)) {
                    under.insert(b);
                }
            }
            if (under.empty()) {
                continue;
            }
            std::unordered_map<u16, bool> known;
            const auto unread_if_off = [&](u16 word) {
                if (const auto it = known.find(word); it != known.end()) {
                    return it->second;
                }
                const auto list = readers.find(word);
                const bool unread =
                    word != condition.word && list != readers.end() &&
                    std::ranges::all_of(list->second, [&](const IR::Inst* read) {
                        return OnlyDescriptorUses(read, under, users);
                    });
                known.emplace(word, unread);
                return unread;
            };
            DescriptorGuard guard{
                .word = condition.word,
                .value = condition.value,
                .taken_if_equal = true_edge ? condition.true_if_equal : !condition.true_if_equal,
            };
            for (const auto& resource : resources) {
                if (!users.contains(resource.user) || !under.contains(resource.user->GetParent())) {
                    continue;
                }
                for (const auto& sharp : resource.sharps) {
                    // All of a descriptor's words or none: half a descriptor is another one. A
                    // word the shader holds itself (an immediate) can't be emptied, so neither are
                    // the others: a T# whose address and format are immediates and whose size is
                    // loaded would be a valid 1x1 image at the same address.
                    std::vector<u16> words;
                    bool whole = true;
                    for (u32 i = 0; i < sharp.num_dwords && whole; ++i) {
                        if (sharp.dwords[i].IsImmediate()) {
                            whole = false;
                            break;
                        }
                        const u16 word = FlatWord(sharp.dwords[i].TryInst());
                        whole = word != 0 && unread_if_off(word);
                        words.push_back(word);
                    }
                    if (whole) {
                        guard.descriptor_words.insert(guard.descriptor_words.end(), words.begin(),
                                                      words.end());
                    }
                }
            }
            auto& words = guard.descriptor_words;
            std::ranges::sort(words);
            const auto repeated = std::ranges::unique(words);
            words.erase(repeated.begin(), repeated.end());
            if (!words.empty()) {
                guards.push_back(std::move(guard));
            }
        }
    }
    return guards;
}

} // namespace Shader::Optimization
