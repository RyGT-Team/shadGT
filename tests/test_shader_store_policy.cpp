// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <algorithm>
#include <array>
#include <fstream>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include "shader_recompiler/lane_read_policy.h"
#include "video_core/shader_store_policy.h"
#include "video_core/renderer_vulkan/shader_fix_scan_cache.h"

TEST(ShaderStorePolicy, ProgramFixScansAreReusedAndComputeScansAreLazy) {
    Vulkan::ShaderFixScanCache cache;
    unsigned base_calls = 0, compute_calls = 0;
    const auto base = [&] { ++base_calls; return 17u; };
    const auto compute = [&] { ++compute_calls; return std::pair{true, false}; };
    EXPECT_EQ(cache.Get(9, false, base, compute).base_fixes, 17u);
    EXPECT_EQ(cache.Get(9, false, base, compute).base_fixes, 17u);
    EXPECT_EQ(base_calls, 1u);
    EXPECT_EQ(compute_calls, 0u);
    EXPECT_TRUE(cache.Get(9, true, base, compute).lane_reads);
    EXPECT_TRUE(cache.Get(9, true, base, compute).lane_reads);
    EXPECT_EQ(base_calls, 1u);
    EXPECT_EQ(compute_calls, 1u);
    cache.Get(10, true, base, compute);
    EXPECT_EQ(base_calls, 2u);
    EXPECT_EQ(compute_calls, 2u);
}

TEST(ShaderStorePolicy, StoredBoundaryAndLegacyControlActualScansAcrossComputeVariants) {
    Vulkan::ShaderFixScanCache cache;
    unsigned base_calls = 0, compute_calls = 0;
    const auto base = [&] { ++base_calls; return 16u; };
    const auto compute = [&] { ++compute_calls; return std::pair{true, true}; };
    // A fresh translation uses the current lowering regardless of the skipped compatibility scan.
    EXPECT_EQ(cache.GetIfNeeded(1, true, false, true, 0, base, compute), nullptr);
    EXPECT_EQ(base_calls, 0u);
    EXPECT_EQ(compute_calls, 0u);
    EXPECT_EQ(Shader::LaneReadFixMarker(true, 32, 64, true), 2048u);
    EXPECT_EQ(Shader::LaneReadFixMarker(true, 32, 128, true), 0u);
    // A stored program first seen at 32 threads can later require the 64-thread policy.
    const auto* stored = cache.GetIfNeeded(2, true, false, true, 3, base, compute);
    ASSERT_NE(stored, nullptr);
    EXPECT_TRUE(stored->lane_reads);
    EXPECT_EQ(cache.GetIfNeeded(2, false, false, true, 3, base, compute), stored);
    EXPECT_EQ(base_calls, 1u);
    EXPECT_EQ(compute_calls, 1u);
    // D10 must classify even if this store did not list a matching program.
    ASSERT_NE(cache.GetIfNeeded(1, false, true, true, 0, base, compute), nullptr);
    EXPECT_EQ(base_calls, 2u);
    EXPECT_EQ(compute_calls, 2u);
    EXPECT_EQ(cache.GetIfNeeded(2, false, false, false, 3, base, compute), nullptr);
}

TEST(ShaderStorePolicy, LaneReadSwitchKeepsDefaultAndOffStoresSeparate) {
    EXPECT_TRUE(Shader::LaneReadUnbranchPolicy(nullptr));
    EXPECT_TRUE(Shader::LaneReadUnbranchPolicy(""));
    EXPECT_TRUE(Shader::LaneReadUnbranchPolicy("1"));
    EXPECT_FALSE(Shader::LaneReadUnbranchPolicy("0"));
    // The marker is independent of the separate loop-optimization feature switch.
    EXPECT_EQ(Shader::LaneReadFixMarker(true, 32, 64, true), 2048u);
    EXPECT_EQ(Shader::LaneReadFixMarker(true, 32, 64, false), 0u);
    EXPECT_EQ(Shader::LaneReadFixMarker(true, 32, 128, true), 0u);
    const auto parent = std::filesystem::temp_directory_path() /
        ("ryty-lane-policy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(parent));
    const auto original = parent / "store";
    ASSERT_TRUE(std::filesystem::create_directory(original));
    { std::ofstream(original / "store-version.txt") << "base\n"; }
    { std::ofstream(original / "sentinel") << "preserve"; }
    const std::string on = "base" + std::string(Shader::LaneReadStoreSuffix(true));
    const std::string off = "base" + std::string(Shader::LaneReadStoreSuffix(false));
    const auto enabled = VideoCore::OpenCompatibleShaderStore(original, on);
    const auto disabled = VideoCore::OpenCompatibleShaderStore(original, off);
    ASSERT_TRUE(enabled);
    ASSERT_TRUE(disabled);
    EXPECT_EQ(*enabled, original);
    EXPECT_NE(*disabled, original);
    EXPECT_EQ(VideoCore::OpenCompatibleShaderStore(original, off), disabled);
    EXPECT_EQ(VideoCore::OpenCompatibleShaderStore(original, on), enabled);
    EXPECT_TRUE(std::filesystem::exists(original / "sentinel"));
    std::filesystem::remove(*disabled / "store-version.txt");
    std::filesystem::remove(*disabled);
    std::filesystem::remove(original / "store-version.txt");
    std::filesystem::remove(original / "sentinel");
    std::filesystem::remove(original);
    std::filesystem::remove(parent);
}

TEST(ShaderStorePolicy, PreservesOriginalAndReusesCompatibleSibling) {
    const auto parent=std::filesystem::temp_directory_path() /
        ("ryty-store-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(parent));
    const auto original=parent/"shader-store";
    ASSERT_TRUE(std::filesystem::create_directory(original));
    { std::ofstream(original/"store-version.txt") << "old-policy\n"; }
    { std::ofstream(original/"sentinel.bin") << "keep"; }
    { std::ofstream(parent/"shaders-pack.txt") << "pack"; }
    { std::ofstream(parent/"game.else-scope-fix") << "old-marks"; }
    const auto same=VideoCore::OpenCompatibleShaderStore(original,"old-policy");
    ASSERT_TRUE(same); EXPECT_EQ(*same,original);
    const auto next=VideoCore::OpenCompatibleShaderStore(original,"new-policy");
    ASSERT_TRUE(next); EXPECT_NE(*next,original);
    EXPECT_EQ(VideoCore::ShaderStoreMarksPath(parent,"game",original,false),parent/"game.else-scope-fix");
    EXPECT_EQ(VideoCore::ShaderStoreMarksPath(parent,"game",*next,true),*next/"else-scope-fix");
    { std::ofstream(VideoCore::ShaderStoreMarksPath(parent,"game",*next,true)) << "new-marks"; }
    std::string old_marks;
    { std::ifstream input(parent/"game.else-scope-fix"); std::getline(input,old_marks); }
    EXPECT_EQ(old_marks,"old-marks");
    { std::ofstream(*next/"new.bin") << "new"; }
    const auto again=VideoCore::OpenCompatibleShaderStore(original,"new-policy");
    ASSERT_TRUE(again); EXPECT_EQ(*again,*next);
    EXPECT_TRUE(std::filesystem::exists(original/"sentinel.bin"));
    EXPECT_TRUE(std::filesystem::exists(parent/"shaders-pack.txt"));
    const auto third=VideoCore::OpenCompatibleShaderStore(original,"third-policy");
    ASSERT_TRUE(third); EXPECT_NE(*third,*next); EXPECT_NE(*third,original);
    EXPECT_TRUE(std::filesystem::exists(*next/"new.bin"));
    std::filesystem::remove(original/"sentinel.bin");
    std::filesystem::remove(original/"store-version.txt");
    std::filesystem::remove(*next/"new.bin");
    std::filesystem::remove(*next/"else-scope-fix");
    std::filesystem::remove(*next/"store-version.txt");
    std::filesystem::remove(*third/"store-version.txt");
    std::filesystem::remove(parent/"shaders-pack.txt");
    std::filesystem::remove(parent/"game.else-scope-fix");
    std::filesystem::remove(original); std::filesystem::remove(*next);
    std::filesystem::remove(*third); std::filesystem::remove(parent);
}

TEST(ShaderStorePolicy, NonDirectoryAndWriteFailureLeaveExistingFileUntouched) {
    const auto parent=std::filesystem::temp_directory_path() /
        ("ryty-store-invalid-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(parent));
    const auto file=parent/"not-a-directory";
    { std::ofstream(file) << "keep"; }
    EXPECT_FALSE(VideoCore::OpenCompatibleShaderStore(file/"child","policy"));
#ifdef _WIN32
    EXPECT_FALSE(VideoCore::OpenCompatibleShaderStore(parent/"invalid?name","policy"));
#endif
    std::string content; { std::ifstream input(file); std::getline(input,content); }
    EXPECT_EQ(content,"keep");
    std::filesystem::remove(file); std::filesystem::remove(parent);
}

TEST(ShaderStorePolicy, ConcurrentSamePolicyCreatorsPreserveOriginalAndReuseACompatibleStore) {
    const auto parent=std::filesystem::temp_directory_path() /
        ("ryty-store-race-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(parent));
    const auto original=parent/"store";
    ASSERT_TRUE(std::filesystem::create_directory(original));
    { std::ofstream(original/"store-version.txt") << "old\n"; }
    { std::ofstream(original/"sentinel") << "keep"; }
    std::array<std::optional<std::filesystem::path>,8> choices;
    std::vector<std::jthread> threads;
    for (size_t i=0;i<choices.size();++i)
        threads.emplace_back([&,i] { choices[i]=VideoCore::OpenCompatibleShaderStore(original,"new"); });
    threads.clear();
    for (const auto& choice:choices) ASSERT_TRUE(choice);
    const auto again=VideoCore::OpenCompatibleShaderStore(original,"new");
    ASSERT_TRUE(again);
    EXPECT_TRUE(std::ranges::any_of(choices,[&](const auto& choice) { return *choice==*again; }));
    std::string content; { std::ifstream input(original/"sentinel"); std::getline(input,content); }
    EXPECT_EQ(content,"keep");
    for (const auto& choice:choices) {
        std::filesystem::remove(*choice/"store-version.txt");
        std::filesystem::remove(*choice);
    }
    std::filesystem::remove(original/"sentinel");
    std::filesystem::remove(original/"store-version.txt");
    std::filesystem::remove(original); std::filesystem::remove(parent);
}
