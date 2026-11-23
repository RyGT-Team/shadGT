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

// A T# of a 64x64 2D image whose address, format and type the shader holds as immediates (words
// 0, 1 and 3) and whose size it loads from the table (word 2), as the review of PR 106 built it.
constexpr u64 MixedAddress = 0x12345600;
std::array<u32, 8> MixedTsharp() {
    auto image = AmdGpu::Image::Null(false);
    image.base_address = MixedAddress >> 8;
    image.width = 63;
    image.height = 63;
    std::array<u32, 8> words{};
    static_assert(sizeof(image) == sizeof(words));
    std::memcpy(words.data(), &image, sizeof(words));
    return words;
}
bool MixedImmediate(u32 word) {
    return word == 0 || word == 1 || word == 3;
}

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
    bool mixed_tsharp = false; ///< the T# is MixedTsharp: immediates and loaded words
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
    Shader::Optimization::ResourceDiscoveryList resources;

    /// Guest addresses the GPU wrote for the draw, as the renderer's probe answers
    /// (Rasterizer::FlatSourcesMayChange), and how often it was asked.
    struct GpuWrites {
        std::vector<u64> addresses;
        u32 asked = 0;
    } gpu;
    static bool GpuMayHaveWritten(const void* context, std::span<const u64> sources) {
        auto& writes = *static_cast<GpuWrites*>(const_cast<void*>(context));
        ++writes.asked;
        for (size_t i = Shader::NUM_USER_DATA_REGS; i < sources.size(); ++i) {
            if (std::ranges::find(writes.addresses, sources[i]) != writes.addresses.end()) {
                return true;
            }
        }
        return false;
    }
    Shader::FlatSourcesProbe probe{&GpuMayHaveWritten, &gpu};

    void TearDown() override {
        Shader::g_flat_sources_probe.store(nullptr);
    }

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
        const auto tsharp_word = [&](u32 i) -> IR::U32 {
            return shape.mixed_tsharp && MixedImmediate(i) ? ir.Imm32(MixedTsharp()[i]) : word[i];
        };
        const auto handle = ir.ImageHandle(
            ir.CompositeConstruct(tsharp_word(0), tsharp_word(1), tsharp_word(2), tsharp_word(3)),
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

    /// entry -> (word decides) body -> merge, as the shape says; then the pass. `replayed`: a shader
    /// record's words (PB-1); `logged`: the walker's output as a read log holds it (PERF-GT7-117).
    void Build(const Shape& shape, const std::vector<u32>* replayed = nullptr,
               Common::GuestReadLog* logged = nullptr) {
        const auto address = reinterpret_cast<u64>(table.data());
        std::memcpy(user_data.data(), &address, sizeof(address));
        for (u32 i = 0; i < DescriptorWords; ++i) {
            table[i] = 0x3f800000U + i; // what an unused slot is left holding: floats
        }
        if (shape.mixed_tsharp) {
            const auto mixed = MixedTsharp();
            for (u32 i = 0; i < 8; ++i) {
                if (!MixedImmediate(i)) {
                    table[TsharpAt + i] = mixed[i];
                }
            }
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
        resources = Shader::Optimization::ResourceDiscoverPass(program, Shader::Profile{});
        Shader::ReplayInputs replay{};
        replay.flat = replayed;
        Shader::t_replay = replayed ? &replay : nullptr;
        Common::t_guest_read_log = logged;
        Shader::Optimization::FlattenExtendedUserdataPass(program, resources);
        Common::t_guest_read_log = nullptr;
        Shader::t_replay = nullptr;
        ASSERT_EQ(descriptor.size(), DescriptorWords);
    }

    /// A read log in replay whose one read is the walker's output: `words`, every one `value`.
    static Common::GuestReadLog LoggedFlat(u32 words, u32 value) {
        Common::GuestReadLog log;
        const std::vector<u32> flat(words, value);
        log.Append(Common::FlatBufReadTag, flat.data(), flat.size() * sizeof(u32));
        log.replay = true;
        return log;
    }

    void Walk(u32 flag) {
        table[FlagAt] = flag;
        info.RefreshFlatBuf();
        // The words the walker and its caller talk through are zero again.
        EXPECT_EQ(info.flattened_ud_src[Shader::SrtWalkerNoGuards], 0U);
        EXPECT_EQ(info.flattened_ud_src[Shader::SrtWalkerGuardEmptied], 0U);
    }

    /// The sampled T# as the draw binds it: what ImageResource::GetSharp reads from the walked
    /// buffer, with the fetch the resource patching pass makes (ConstructSharpFetch).
    AmdGpu::Image BoundTsharp() const {
        const auto found = std::ranges::find_if(resources, [](const auto& resource) {
            return resource.user && resource.user->GetOpcode() == IR::Opcode::ImageSampleRaw;
        });
        EXPECT_NE(found, resources.end());
        Shader::ImageResource image{};
        const auto& sharp = found->sharps[0];
        for (u32 i = 0; i < sharp.num_dwords; ++i) {
            if (sharp.dwords[i].IsImmediate()) {
                image.sharp_fetch.immediates[i] = sharp.dwords[i].U32();
            } else {
                image.sharp_fetch.offsets[i] = sharp.dwords[i].Inst()->Flags<u16>();
                image.sharp_fetch.load_mask |= u8(1U << i);
            }
        }
        return image.GetSharp(info);
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

TEST_F(DescriptorGuards, ARecordsReplayIsTranslatedFromItsWords) {
    // PB-1 rebuilds a record's missing SPIR-V from its words and stores it under that record,
    // whose walker and specialization were made from them. Here a record older than the guards,
    // with the branch off (every word is junk, the flag's too). Emptied, the code was made for
    // the null 2D image while the record kept the image its junk T# decoded to (a cube).
    const std::vector<u32> recorded(96, 0x3f800000U);
    Build({.equal = true, .constant = 0xdeadbeefU}, &recorded);
    for (IR::Inst* word : descriptor) {
        EXPECT_EQ(info.flattened_ud_buf[word->Flags<u16>()], 0x3f800000U);
    }
}

TEST_F(DescriptorGuards, AReadLogsReplayIsEmptiedAsTheWalkerWould) {
    // The manifest pre-translation replays a record's read log, maybe made before the guards
    // (every word junk, the flag's too), and stores the translation as a new permutation with this
    // walker: its draws empty these words, so must its translation.
    // 16 user data words, the flag and the 12 descriptor words: the walker's output.
    auto log = LoggedFlat(Shader::NUM_USER_DATA_REGS + 1 + DescriptorWords, 0x3f800000U);
    Build({.equal = true, .constant = 0xdeadbeefU}, nullptr, &log);
    ASSERT_EQ(log.misses, 0U) << "the walker's output is not the size the log holds";
    ASSERT_EQ(log.next, 1U);
    ExpectEmpty();
}

TEST_F(DescriptorGuards, ReplayWithTheBranchOnIsLeftAlone) {
    const std::vector<u32> recorded(96, 0x3f800000U);
    Build({.equal = true, .constant = 0x3f800000U}, &recorded);
    for (IR::Inst* word : descriptor) {
        EXPECT_EQ(info.flattened_ud_buf[word->Flags<u16>()], 0x3f800000U);
    }
}

TEST_F(DescriptorGuards, AMixedTsharpIsKeptWhole) {
    // Its loaded words emptied, the T# kept its immediate address, format and type: a valid
    // 1x1 image at the address of the 64x64 one.
    Build({.mixed_tsharp = true});
    Walk(1);
    const auto on = BoundTsharp();
    ASSERT_TRUE(on.Valid());
    EXPECT_EQ(on.Address(), MixedAddress);
    EXPECT_EQ(on.width + 1, 64U);
    EXPECT_EQ(on.height + 1, 64U);
    Walk(0);
    const auto off = BoundTsharp();
    EXPECT_FALSE(off.Valid() && off.Address() == MixedAddress &&
                 std::memcmp(&off, &on, sizeof(on)) != 0)
        << "a " << off.width + 1 << "x" << off.height + 1 << " image at the same address";
    EXPECT_EQ(std::memcmp(&off, &on, sizeof(on)), 0);
    // The S#, which the shader loads whole, is still emptied.
    for (u32 i = 8; i < DescriptorWords; ++i) {
        EXPECT_EQ(info.flattened_ud_buf[descriptor[i]->Flags<u16>()], 0U) << i;
    }
}

TEST_F(DescriptorGuards, AFlagTheGpuMayWriteKeepsTheDescriptors) {
    // The flag's memory was written by GPU work before the draw: its binding copies the word
    // again (RefreshGpuWrittenConstants) and the shader may take the branch. Emptied, it would
    // sample nothing where the game's texture is.
    Build({});
    gpu.addresses = {reinterpret_cast<u64>(table.data() + FlagAt)};
    Shader::g_flat_sources_probe.store(&probe);
    Walk(0);
    ExpectCopied();
    EXPECT_EQ(gpu.asked, 1U);
}

TEST_F(DescriptorGuards, TheGpuIsAskedOnlyWhenAGuardEmpties) {
    Build({});
    gpu.addresses = {reinterpret_cast<u64>(table.data() + 60)}; // not read by the shader
    Shader::g_flat_sources_probe.store(&probe);
    Walk(1);
    ExpectCopied();
    EXPECT_EQ(gpu.asked, 0U);
    Walk(0);
    ExpectEmpty();
    EXPECT_EQ(gpu.asked, 1U);
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
