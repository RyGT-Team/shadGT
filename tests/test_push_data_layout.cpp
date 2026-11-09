// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <gtest/gtest.h>
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/resource.h"

void Common::Log::VLog(Class, Level, const char* file, int line, const char*, fmt::string_view,
                       fmt::format_args) {
    ADD_FAILURE() << "Unexpected push-layout assertion at " << file << ':' << line;
}
void assert_fail_impl() { std::abort(); }
void unreachable_impl() { std::abort(); }

TEST(PushDataLayout, GraphicsStageRangesDoNotOverlapOrReachMask) {
    std::array<bool, 96> occupied{};
    Shader::PushData push{};
    for (u32 stage = 0; stage < Shader::Backend::MaxDescriptorSets; ++stage) {
        const auto begin = Shader::Backend::StageBufferOffsetBase[stage];
        const auto count = Shader::Backend::StageBufferOffsetCount[stage];
        ASSERT_LE(begin + count, push.buf_offsets.size());
        for (u32 i = begin; i < begin + count; ++i) {
            EXPECT_FALSE(occupied[i]);
            occupied[i] = true;
            push.AddOffset(i, i + 1);
        }
    }
    for (u32 i = 0; i < push.buf_offsets.size(); ++i) EXPECT_EQ(push.buf_offsets[i], i + 1);
    EXPECT_EQ(push.quad_sample_mask, (std::array<u32, 2>{~0u, ~0u}));
    EXPECT_EQ(offsetof(Shader::PushData, quad_sample_mask), 112u);
    EXPECT_EQ(sizeof(push), 120u);
    EXPECT_EQ(Shader::PushData::LdsScratchIndex, Shader::PushData::QuadSampleMaskIndex);
}

TEST(PushDataLayout, ComputeCanUseLastExpandedOffsetWithoutChangingMask) {
    Shader::PushData push{};
    EXPECT_EQ(Shader::Backend::StageStart(5).buffer, 0u);
    EXPECT_EQ(Shader::Backend::StageBufferOffsetCount[5], 96u);
    push.AddOffset(95, 255);
    EXPECT_EQ(push.buf_offsets[95], 255);
    EXPECT_EQ(push.quad_sample_mask, (std::array<u32, 2>{~0u, ~0u}));
}
