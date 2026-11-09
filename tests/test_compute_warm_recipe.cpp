// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <chrono>
#include <array>
#include <filesystem>
#include <fstream>
#include <thread>
#include "video_core/renderer_vulkan/compute_warm_recipe.h"

namespace {
constexpr u64 Sig = 0x1122334455667788ull;
std::vector<u32> ValidRecipe() {
    return {0x43574752, 2, u32(Sig), u32(Sig >> 32), 0, 0, 0, 1,
            0, 7, 1, 6, 0x07230203, 0x00010000, 0, 1, 0, 0x00010038};
}
bool Valid(const std::vector<u32>& words) {
    return Vulkan::ValidateComputeWarmRecipe(words, Sig, 32, 0x00010600, true, true);
}
}

TEST(ComputeWarmRecipe, AcceptsMatchingBoundedStructure) {
    EXPECT_TRUE(Valid(ValidRecipe()));
}

TEST(ComputeWarmRecipe, RejectsTruncationAndMismatchedPolicy) {
    auto words = ValidRecipe();
    for (size_t length = 0; length < words.size(); ++length)
        EXPECT_FALSE(Valid(std::vector<u32>(words.begin(), words.begin() + length)));
    words = ValidRecipe(); words[2] ^= 1; EXPECT_FALSE(Valid(words));
    words = ValidRecipe(); words[1] = 1; EXPECT_FALSE(Valid(words));
    words = ValidRecipe(); words[12] = 0; EXPECT_FALSE(Valid(words));
    words = ValidRecipe(); words[13] = 0x00010700; EXPECT_FALSE(Valid(words));
}

TEST(ComputeWarmRecipe, RejectsMalformedBindingsAndUnsupportedSubgroup) {
    auto words = ValidRecipe(); words[8] = 1; EXPECT_FALSE(Valid(words));
    words = ValidRecipe(); words[9] = 42; EXPECT_FALSE(Valid(words));
    words = ValidRecipe(); words[10] = 0; EXPECT_FALSE(Valid(words));
    words = ValidRecipe(); words[7] = 257; EXPECT_FALSE(Valid(words));
    words = ValidRecipe(); words[5] = 1; words[6] = 16; EXPECT_FALSE(Valid(words));
    words = ValidRecipe(); words[4] = 1;
    EXPECT_FALSE(Vulkan::ValidateComputeWarmRecipe(words, Sig, 1, 0x00010600, true, true));
}

TEST(ComputeWarmRecipe, MissingDirectoryAndStalePolicyDoNotAbortOrReplaceAnything) {
    const auto root=std::filesystem::temp_directory_path() /
        ("ryty-warm-recipe-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    EXPECT_TRUE(Vulkan::ListComputeWarmRecipes(root, "-current").empty());
    ASSERT_TRUE(std::filesystem::create_directory(root));
    { std::ofstream(root / "old-other.bin") << "preserve"; }
    { std::ofstream(root / "small-current.bin") << "x"; }
    { std::ofstream(root / "large-current.bin") << "12345"; }
    const auto paths=Vulkan::ListComputeWarmRecipes(root, "-current");
    ASSERT_EQ(paths.size(),2u);
    EXPECT_EQ(paths[0].filename(),"large-current.bin");
    EXPECT_TRUE(std::filesystem::exists(root / "old-other.bin"));
    std::filesystem::remove(root / "old-other.bin");
    std::filesystem::remove(root / "small-current.bin");
    std::filesystem::remove(root / "large-current.bin");
    std::filesystem::remove(root);
    EXPECT_TRUE(Vulkan::ListComputeWarmRecipes(root, "-current").empty());
}

TEST(ComputeWarmRecipe, ConcurrentWritersNeverReplaceExistingRecipe) {
    const auto root=std::filesystem::temp_directory_path() /
        ("ryty-warm-write-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    const auto file=root/"recipe.bin";
    const std::array<u32,3> first{1,2,3}, second{4,5,6};
    bool a=false,b=false;
    std::jthread one([&] { a=Vulkan::WriteComputeWarmRecipeExclusive(file,first); });
    std::jthread two([&] { b=Vulkan::WriteComputeWarmRecipeExclusive(file,second); });
    one.join(); two.join();
    EXPECT_NE(a,b);
    std::array<u32,3> stored{};
    { std::ifstream in(file,std::ios::binary); in.read(reinterpret_cast<char*>(stored.data()),sizeof(stored)); EXPECT_TRUE(in); }
    EXPECT_TRUE(stored==first || stored==second);
    EXPECT_FALSE(Vulkan::WriteComputeWarmRecipeExclusive(file,first));
    std::filesystem::remove(file); std::filesystem::remove(root);
}
