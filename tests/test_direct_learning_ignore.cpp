// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// FIX-SGT-119: the pre-translation replays a record "as a store made before the learning did":
// IgnoreLearned turns the learned decisions off for this thread only, for its own lifetime.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <gtest/gtest.h>
#include "common/types.h"
#include "shader_recompiler/direct_learning.h"

namespace DL = Shader::DirectLearning;

namespace {

constexpr u64 Learned = 0xdeadbeef;
constexpr u64 TooFewBinds = 0xfeedf00d;

class DirectLearningIgnore : public testing::Test {
protected:
    static void SetUpTestSuite() {
        // FIX-SGT-202: the learning is off unless SHADGT_LEARNED_DIRECT=1; set before Load reads it.
#ifdef _WIN32
        _putenv_s("SHADGT_LEARNED_DIRECT", "1");
#else
        setenv("SHADGT_LEARNED_DIRECT", "1", 1);
#endif
        // ctest runs each test in its own process, side by side: one file per process.
        const auto path = std::filesystem::temp_directory_path() /
                          ("rygt-direct-reads-test-" + std::to_string(std::random_device{}()) + ".txt");
        {
            std::ofstream out(path);
            out << "# test\n";
            out << "00000000deadbeef 64 1\n"; // slot 0 read in place, slot 1 never
            out << "00000000feedf00d 8 0\n";  // under MinBinds: no decision yet
        }
        DL::Load(path);
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

// FIX-SGT-202: only SHADGT_LEARNED_DIRECT=1 turns the learning on.
TEST(DirectLearningSwitch, OffUnlessSetToOne) {
    EXPECT_FALSE(DL::EnabledFrom(nullptr));
    EXPECT_FALSE(DL::EnabledFrom(""));
    EXPECT_FALSE(DL::EnabledFrom("0"));
    EXPECT_TRUE(DL::EnabledFrom("1"));
}

TEST_F(DirectLearningIgnore, LearnedDecisionsApplyWhenOn) {
    EXPECT_FALSE(DL::NoDirect(Learned, 0));
    EXPECT_TRUE(DL::NoDirect(Learned, 1));
    EXPECT_FALSE(DL::NoDirect(TooFewBinds, 1));
}

TEST_F(DirectLearningIgnore, IgnoredWhileTheGuardLives) {
    {
        DL::IgnoreLearned every_direct_path;
        EXPECT_FALSE(DL::NoDirect(Learned, 1));
    }
    EXPECT_TRUE(DL::NoDirect(Learned, 1));
}

TEST_F(DirectLearningIgnore, NestedGuardsRestoreTheOuterOne) {
    DL::IgnoreLearned outer;
    {
        DL::IgnoreLearned inner;
        EXPECT_FALSE(DL::NoDirect(Learned, 1));
    }
    EXPECT_FALSE(DL::NoDirect(Learned, 1));
}

TEST_F(DirectLearningIgnore, OtherThreadsKeepTheLearnedDecisions) {
    DL::IgnoreLearned every_direct_path;
    bool other = false;
    std::thread([&] { other = DL::NoDirect(Learned, 1); }).join();
    EXPECT_TRUE(other);
    EXPECT_FALSE(DL::NoDirect(Learned, 1));
}

} // namespace
