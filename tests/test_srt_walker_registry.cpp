// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <array>
#include <cstring>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/recompiler.h"

// PERF-038: the background pipeline reader decoded shader metadata on its own thread, which
// registers SRT walker code, while the GPU thread did the same; the shared code buffer and its
// map were corrupted (Lance's 20:12 run: a reader spinning at 98% of a core, then a crash).
TEST(SrtWalkerRegistry, ConcurrentRegistrationKeepsEveryCopy) {
    constexpr u32 Threads = 4;
    constexpr u32 PerThread = 3000;
    std::atomic<u32> mismatches{};
    std::vector<std::thread> threads;
    for (u32 t = 0; t < Threads; ++t) {
        threads.emplace_back([&, t] {
            for (u32 i = 0; i < PerThread; ++i) {
                // ret, then the thread and index as padding nobody executes.
                std::array<u8, 12> code{0xC3};
                const u64 tag = (u64(t) << 32) | i;
                std::memcpy(code.data() + 4, &tag, sizeof(tag));
                const u64 key = tag + 1;
                auto walker = Shader::RegisterWalkerCode(code.data(), code.size(), key);
                const auto* func = reinterpret_cast<const u8*>(walker.function);
                if (!walker.owner || !func || std::memcmp(func, code.data(), code.size()) != 0) {
                    ++mismatches;
                    continue;
                }
                // Independent owners must never share mutable executable code.
                auto again = Shader::RegisterWalkerCode(code.data(), code.size(), key);
                if (!again.owner || !again.function || again.function == walker.function ||
                    std::memcmp(reinterpret_cast<const u8*>(again.function), code.data(), code.size()) != 0) {
                    ++mismatches;
                }
                walker.function(nullptr, nullptr, nullptr);
                again.function(nullptr, nullptr, nullptr);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(mismatches.load(), 0u);
}

TEST(SrtWalkerRegistry, LegacyPersistentLayoutRemains24Bytes) {
    EXPECT_EQ(sizeof(Shader::PersistentSrtInfo), 24u);
    EXPECT_EQ(offsetof(Shader::PersistentSrtInfo, walker_func), 0u);
    EXPECT_EQ(offsetof(Shader::PersistentSrtInfo, walker_func_size), 8u);
    EXPECT_EQ(offsetof(Shader::PersistentSrtInfo, flattened_bufsize_dw), 16u);
}

TEST(SrtWalkerRegistry, EmittedWalkerSurvivesInfoCopiesAndChurn) {
    Shader::Pools pools;
    Shader::Info info{};
    Shader::IR::Program program{info};
    auto* block = pools.block_pool.Create(pools.inst_pool);
    Shader::IR::IREmitter ir{*block};
    program.blocks.push_back(block);
    program.post_order_blocks.push_back(block);
    std::array<u32, 16> user_data{};
    std::array<u32, 4> table{11, 22, 33, 44};
    const auto address = reinterpret_cast<u64>(table.data());
    std::memcpy(user_data.data(), &address, sizeof(address));
    info.user_data = user_data;
    const auto base = ir.CompositeConstruct(ir.GetUserData(Shader::IR::ScalarReg::S0),
                                            ir.GetUserData(Shader::IR::ScalarReg::S1));
    const auto read = ir.ReadConst(base, ir.Imm32(2U));
    Shader::Optimization::FlattenExtendedUserdataPass(program);
    const auto offset = read.Inst()->Flags<u16>();
    ASSERT_NE(offset, 0u);
    ASSERT_TRUE(info.srt_walker_owner);
    Shader::Info copied = info;
    info.srt_walker_owner.reset();
    std::array<u8, 1> ret{0xc3};
    for (int i = 0; i < 10000; ++i) {
        auto churn = Shader::RegisterWalkerCode(ret.data(), ret.size());
        ASSERT_TRUE(churn.owner);
    }
    table[2] = 99;
    copied.RefreshFlatBuf();
    EXPECT_EQ(copied.flattened_ud_buf[offset], 99u);
    EXPECT_EQ(copied.flattened_ud_src[offset], reinterpret_cast<u64>(&table[2]));
}
