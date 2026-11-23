// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// FIX-SGT-115: descriptors read only in a branch that is off. The graph is the one of GT7's
// depth-to-distance pass: a word of the table decides a branch that samples a texture from the
// same table. Every rule that must leave the descriptor alone has its case. As the translator
// does, every loaded word is also written to its register, and SSA runs before the pass.

#include <array>
#include <cstring>
#include <optional>
#include <vector>
#include <gtest/gtest.h>

#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/descriptor_guards.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"

namespace {

namespace IR = Shader::IR;

// Where the parts are in the guest's table, in dwords.
constexpr u32 TsharpAt = 0;
constexpr u32 SsharpAt = 8;
constexpr u32 FlagAt = 40;
constexpr u32 SecondFlagAt = 41;
constexpr u32 DescriptorWords = 12; // a T# and an S#

struct Shape {
    bool equal = false;           ///< the branch compares with IEqual instead of INotEqual
    bool negated = false;         ///< through a LogicalNot
    bool constant_first = true;   ///< which side the constant is on
    u32 constant = 0;             ///< what the word is compared with
    bool sample_in_merge = false; ///< the sample is past the branch
    bool loop = false;            ///< the branch's body is a loop
    bool body_also_from_entry_side = false; ///< a second path reaches the body without the branch
    bool used_as_data = false;    ///< a word of the T# is also computed with
    bool sampled_again_outside = false; ///< the same descriptor is sampled past the branch too
    bool storage = false;         ///< the image is written
    bool unknown_condition = false; ///< the branch compares two words of the table
    bool tess_stage = false;
    bool on_false_edge = false; ///< the body is where the branch falls through
    bool register_read_in_merge = false; ///< a word of the T# reaches past the branch by a register
    bool loaded_before_branch = false;   ///< the words are in registers before the branch decides
    bool no_registers = false;           ///< reads that no register write goes with
    bool register_read_in_loop = false;  ///< the loop's head computes with a word of the round before
};

class DescriptorGuards : public testing::Test {
protected:
    Shader::Pools pools;
    Shader::Info info{};
    IR::Program program{info};
    std::array<u32, 16> user_data{};
    std::array<u32, 64> table{};
    std::array<Shader::Gcn::Block, 5> cfg{};
    std::vector<IR::Inst*> descriptor;

    IR::Block* NewBlock() {
        auto* block = pools.block_pool.Create(pools.inst_pool);
        program.blocks.push_back(block);
        return block;
    }

    void Conditional(IR::Block* from, Shader::Gcn::Block& node, IR::Block* taken,
                     Shader::Gcn::Block& taken_node, IR::Block* other,
                     Shader::Gcn::Block& other_node) {
        node.ir_block = from;
        taken_node.ir_block = taken;
        other_node.ir_block = other;
        node.branch_true = &taken_node;
        node.branch_false = &other_node;
        from->cfg_block = &node;
        from->AddBranch(taken);
        from->AddBranch(other);
    }

    void Sample(IR::IREmitter& ir, IR::IREmitter& load, const IR::Value& base, const Shape& shape,
                bool note_words) {
        std::array<IR::U32, DescriptorWords> word;
        for (u32 i = 0; i < DescriptorWords; ++i) {
            word[i] = load.ReadConst(base, load.Imm32(i < 8 ? TsharpAt + i : SsharpAt + i - 8));
            if (!shape.no_registers) {
                load.SetScalarReg(static_cast<IR::ScalarReg>(u32(IR::ScalarReg::S8) + i), word[i]);
            }
            if (note_words) {
                descriptor.push_back(word[i].Inst());
            }
        }
        const auto handle =
            ir.ImageHandle(ir.CompositeConstruct(word[0], word[1], word[2], word[3]),
                           ir.CompositeConstruct(word[4], word[5], word[6], word[7]));
        const auto at = ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(0.0f), ir.Imm32(0.0f),
                                              ir.Imm32(0.0f));
        if (shape.storage) {
            const auto zero =
                ir.CompositeConstruct(ir.Imm32(0U), ir.Imm32(0U), ir.Imm32(0U), ir.Imm32(0U));
            ir.ImageWrite(handle, zero, ir.Imm32(0U), ir.Imm32(0U), at, {});
            return;
        }
        const auto sampler = ir.CompositeConstruct(word[8], word[9], word[10], word[11]);
        static_cast<void>(ir.ImageSampleRaw(handle, sampler, at, at, at, at, {}));
        if (note_words && shape.used_as_data) {
            static_cast<void>(ir.IAdd(word[3], ir.Imm32(1U)));
        }
    }

    /// entry -> (word decides) body -> merge, as the shape says; then the pass.
    void Build(const Shape& shape, const std::vector<u32>* replayed = nullptr) {
        const auto address = reinterpret_cast<u64>(table.data());
        std::memcpy(user_data.data(), &address, sizeof(address));
        for (u32 i = 0; i < DescriptorWords; ++i) {
            table[i] = 0x3f800000U + i; // what an unused slot is left holding: floats
        }
        info.user_data = user_data;
        info.sw_stage =
            shape.tess_stage ? Shader::SwStage::TessellationEval : Shader::SwStage::Fragment;

        IR::Block* const first = NewBlock();
        IR::Block* const branch = shape.body_also_from_entry_side ? NewBlock() : first;
        IR::Block* const body = NewBlock();
        IR::Block* const again = shape.loop ? NewBlock() : nullptr;
        IR::Block* const merge = NewBlock();

        IR::IREmitter head{*first};
        const auto base = head.CompositeConstruct(head.GetUserData(IR::ScalarReg::S0),
                                                  head.GetUserData(IR::ScalarReg::S1));
        if (branch != first) {
            // The first branch goes to the body directly, whatever the table says.
            first->branch_cond = head.ConditionRef(head.Imm1(true));
            Conditional(first, cfg[0], body, cfg[2], branch, cfg[1]);
        }
        IR::IREmitter ir{*branch};
        const IR::U32 flag = ir.ReadConst(base, ir.Imm32(FlagAt));
        ir.SetScalarReg(IR::ScalarReg::S2, flag);
        const IR::U32 constant = shape.unknown_condition
                                     ? IR::U32{ir.ReadConst(base, ir.Imm32(SecondFlagAt))}
                                     : ir.Imm32(shape.constant);
        const IR::U32 left = shape.constant_first ? constant : flag;
        const IR::U32 right = shape.constant_first ? flag : constant;
        IR::U1 condition = shape.equal ? ir.IEqual(left, right) : ir.INotEqual(left, right);
        if (shape.negated) {
            condition = ir.LogicalNot(condition);
        }
        ir.SetScc(condition);
        branch->branch_cond = ir.ConditionRef(condition);
        if (shape.on_false_edge) {
            Conditional(branch, cfg[1], merge, cfg[4], body, cfg[2]);
        } else {
            Conditional(branch, cfg[1], body, cfg[2], merge, cfg[4]);
        }

        IR::IREmitter in_body{*body};
        IR::IREmitter in_merge{*merge};
        if (shape.loop) {
            IR::IREmitter in_again{*again};
            Sample(in_again, in_again, base, shape, true);
            if (shape.register_read_in_loop) {
                const IR::U32 last_round = in_body.GetScalarReg(IR::ScalarReg::S8);
                static_cast<void>(in_body.IAdd(last_round, in_body.Imm32(1U)));
            }
            body->AddBranch(again);
            again->AddBranch(body); // the back edge
            again->AddBranch(merge);
        } else {
            if (shape.sample_in_merge) {
                Sample(in_merge, in_merge, base, shape, true);
            } else {
                Sample(in_body, shape.loaded_before_branch ? ir : in_body, base, shape, true);
            }
            body->AddBranch(merge);
        }
        if (shape.sampled_again_outside) {
            Sample(in_merge, in_merge, base, shape, false);
        }
        if (shape.register_read_in_merge) {
            const IR::U32 left_there = in_merge.GetScalarReg(IR::ScalarReg::S8);
            static_cast<void>(in_merge.IAdd(left_there, in_merge.Imm32(1U)));
        }

        program.post_order_blocks = IR::PostOrder(first);
        Shader::Optimization::SsaRewritePass(program);
        auto resources = Shader::Optimization::ResourceDiscoverPass(program, Shader::Profile{});
        Shader::ReplayInputs replay{};
        replay.flat = replayed;
        Shader::t_replay = replayed ? &replay : nullptr;
        Shader::Optimization::FlattenExtendedUserdataPass(program, resources);
        Shader::t_replay = nullptr;
        ASSERT_EQ(descriptor.size(), DescriptorWords);
    }

    void Walk(u32 flag) {
        table[FlagAt] = flag;
        info.RefreshFlatBuf();
    }

    void ExpectEmpty() {
        for (u32 i = 0; i < descriptor.size(); ++i) {
            const u16 at = descriptor[i]->Flags<u16>();
            ASSERT_GE(at, Shader::NUM_USER_DATA_REGS);
            EXPECT_EQ(info.flattened_ud_buf[at], 0U) << "descriptor word " << i;
            EXPECT_EQ(info.flattened_ud_src[at], 0U) << "descriptor word " << i;
        }
    }

    void ExpectCopied() {
        for (u32 i = 0; i < descriptor.size(); ++i) {
            const u16 at = descriptor[i]->Flags<u16>();
            ASSERT_GE(at, Shader::NUM_USER_DATA_REGS);
            EXPECT_EQ(info.flattened_ud_buf[at], table[i]) << "descriptor word " << i;
            EXPECT_EQ(info.flattened_ud_src[at], reinterpret_cast<u64>(table.data() + i));
        }
    }

    void ExpectNeverEmptied() {
        for (const u32 flag : {0U, 1U, 7U, 0xffffffffU}) {
            Walk(flag);
            ExpectCopied();
        }
    }
};

TEST_F(DescriptorGuards, BranchOffEmptiesTheDescriptorsOnly) {
    Build({});
    Walk(0);
    ExpectEmpty();
    // The word the branch reads is still given to the shader.
    bool flag_seen = false;
    for (u32 i = 0; i < info.flattened_ud_src.size(); ++i) {
        if (info.flattened_ud_src[i] == reinterpret_cast<u64>(table.data() + FlagAt)) {
            flag_seen = true;
            EXPECT_EQ(info.flattened_ud_buf[i], 0U);
        }
    }
    EXPECT_TRUE(flag_seen);
}

TEST_F(DescriptorGuards, BranchOnCopiesTheDescriptors) {
    Build({});
    for (const u32 flag : {1U, 2U, 0x80000000U}) {
        Walk(flag);
        ExpectCopied();
    }
}

TEST_F(DescriptorGuards, FollowsTheWordAtEveryWalk) {
    Build({});
    Walk(1);
    ExpectCopied();
    Walk(0);
    ExpectEmpty();
    Walk(1);
    ExpectCopied();
}

TEST_F(DescriptorGuards, EqualBranch) {
    Build({.equal = true, .constant = 5});
    Walk(4);
    ExpectEmpty();
    Walk(5);
    ExpectCopied();
}

TEST_F(DescriptorGuards, NegatedBranch) {
    Build({.negated = true, .constant = 3});
    // not (word != 3): taken when the word is 3
    Walk(3);
    ExpectCopied();
    Walk(0);
    ExpectEmpty();
}

TEST_F(DescriptorGuards, ConstantOnTheRight) {
    Build({.constant_first = false});
    Walk(0);
    ExpectEmpty();
    Walk(1);
    ExpectCopied();
}

TEST_F(DescriptorGuards, ValueWithTheTopBit) {
    Build({.equal = true, .constant = 0xbf800000U});
    Walk(0x3f800000U);
    ExpectEmpty();
    Walk(0xbf800000U);
    ExpectCopied();
}

TEST_F(DescriptorGuards, BodyOnTheFalseEdge) {
    Build({.on_false_edge = true});
    // word != 0 jumps over the body: it runs when the word is 0
    Walk(0);
    ExpectCopied();
    Walk(1);
    ExpectEmpty();
}

TEST_F(DescriptorGuards, LoopUnderTheBranch) {
    Build({.loop = true});
    Walk(0);
    ExpectEmpty();
    Walk(1);
    ExpectCopied();
}

TEST_F(DescriptorGuards, ReplayIsEmptiedToo) {
    // A replay fills the buffer without the walker, here from a record older than the guards.
    // The flag's place isn't known before the pass, so every word is junk, the flag's too.
    const std::vector<u32> recorded(96, 0x3f800000U);
    Build({.equal = true, .constant = 0xdeadbeefU}, &recorded);
    ExpectEmpty();
}

TEST_F(DescriptorGuards, ReplayWithTheBranchOnIsLeftAlone) {
    const std::vector<u32> recorded(96, 0x3f800000U);
    Build({.equal = true, .constant = 0x3f800000U}, &recorded);
    for (IR::Inst* word : descriptor) {
        EXPECT_EQ(info.flattened_ud_buf[word->Flags<u16>()], 0x3f800000U);
    }
}

TEST_F(DescriptorGuards, SampledPastTheBranchIsNeverEmptied) {
    Build({.sample_in_merge = true});
    ExpectNeverEmptied();
}

TEST_F(DescriptorGuards, BodyReachedAnotherWayIsNeverEmptied) {
    Build({.body_also_from_entry_side = true});
    ExpectNeverEmptied();
}

TEST_F(DescriptorGuards, SampledAgainOutsideIsNeverEmptied) {
    Build({.sampled_again_outside = true});
    ExpectNeverEmptied();
}

TEST_F(DescriptorGuards, WordComputedWithKeepsTheWholeDescriptor) {
    Build({.used_as_data = true});
    Walk(0);
    // The T# keeps all its words. The S#, which nothing else reads, is emptied.
    for (u32 i = 0; i < 8; ++i) {
        EXPECT_EQ(info.flattened_ud_buf[descriptor[i]->Flags<u16>()], table[i]) << i;
    }
    for (u32 i = 8; i < DescriptorWords; ++i) {
        EXPECT_EQ(info.flattened_ud_buf[descriptor[i]->Flags<u16>()], 0U) << i;
    }
}

TEST_F(DescriptorGuards, WordThatLeavesTheBranchInARegisterKeepsTheWholeDescriptor) {
    // Past the branch a read of the register is a phi of the word and of what was there before.
    Build({.register_read_in_merge = true});
    Walk(0);
    for (u32 i = 0; i < 8; ++i) {
        EXPECT_EQ(info.flattened_ud_buf[descriptor[i]->Flags<u16>()], table[i]) << i;
    }
    for (u32 i = 8; i < DescriptorWords; ++i) {
        EXPECT_EQ(info.flattened_ud_buf[descriptor[i]->Flags<u16>()], 0U) << i;
    }
}

TEST_F(DescriptorGuards, WordCarriedRoundTheLoopKeepsTheWholeDescriptor) {
    // The phi is under the branch, and what it is used for is not a descriptor.
    Build({.loop = true, .register_read_in_loop = true});
    Walk(0);
    for (u32 i = 0; i < 8; ++i) {
        EXPECT_EQ(info.flattened_ud_buf[descriptor[i]->Flags<u16>()], table[i]) << i;
    }
}

TEST_F(DescriptorGuards, SampledPastTheBranchWithoutRegistersIsNeverEmptied) {
    // No register write to give it away: the sample's own block is what keeps it.
    Build({.sampled_again_outside = true, .no_registers = true});
    ExpectNeverEmptied();
}

TEST_F(DescriptorGuards, WordsInRegistersBeforeTheBranchAreNeverEmptied) {
    Build({.loaded_before_branch = true});
    ExpectNeverEmptied();
}

TEST_F(DescriptorGuards, StorageImageIsNeverEmptied) {
    Build({.storage = true});
    ExpectNeverEmptied();
}

TEST_F(DescriptorGuards, OtherConditionsAreNeverUsed) {
    Build({.unknown_condition = true});
    ExpectNeverEmptied();
}

TEST_F(DescriptorGuards, TessellationStagesAreLeftAlone) {
    Build({.tess_stage = true});
    ExpectNeverEmptied();
}

} // namespace
