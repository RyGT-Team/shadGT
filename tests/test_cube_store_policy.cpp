#include <gtest/gtest.h>
#include <array>
#include "video_core/renderer_vulkan/shader_usage_serde.h"

TEST(ShaderStore, CubeEnginePolicyComposesWithEveryBinaryVersion) {
    Shader::Info info{};
    info.hw_stage = Shader::HwStage::Fragment;
    info.sw_stage = Shader::SwStage::Fragment;
    info.images.emplace_back();
    Shader::StageSpecialization spec{};
    spec.info = &info;
    spec.runtime_info.Initialize(info.hw_stage, info.sw_stage);
    spec.images.resize(1);
    spec.images[0].type = AmdGpu::ImageType::Cube;
    // Native cube, single-sample barycentric and discard-kill revisions must
    // each retain their own base version when the classic cube tag is added.
    constexpr u32 cube_version = 279u, bary_version = 282u, kill_version = 285u;
    const std::array<u32, 3> versions{
        Vulkan::FragmentBinaryVersion(false, false, true, kill_version, bary_version, cube_version),
        Vulkan::FragmentBinaryVersion(false, true, true, kill_version, bary_version, cube_version),
        Vulkan::FragmentBinaryVersion(true, true, true, kill_version, bary_version, cube_version),
    };
    EXPECT_EQ(versions, (std::array<u32,3>{cube_version, bary_version, kill_version}));
    EXPECT_EQ(Vulkan::FragmentBinaryVersion(false, false, false,
                                             kill_version, bary_version, cube_version), 0u);
    for (const u32 version : versions) {
        const auto tagged = Vulkan::CubeVersionFor(version, spec, false);
        EXPECT_NE(tagged, version);
        EXPECT_EQ(Vulkan::UntagCubeVersion(tagged), version);
        EXPECT_TRUE(Vulkan::StoredCubesFit(spec, info, true, false));
        EXPECT_FALSE(Vulkan::StoredCubesFit(spec, info, true, true));
        EXPECT_FALSE(Vulkan::StoredCubesFit(spec, info, false, true));
        info.images[0].post_op = Shader::SharpFetchPostOp::NativeCubeSampling;
        EXPECT_TRUE(Vulkan::StoredCubesFit(spec, info, false, true));
        EXPECT_FALSE(Vulkan::StoredCubesFit(spec, info, false, false));
        info.images[0].post_op = {};
    }
    spec.images[0].is_storage = true;
    EXPECT_FALSE(Vulkan::TranslatedWithFaceCubes(spec, false));
    EXPECT_TRUE(Vulkan::StoredCubesFit(spec, info, false, true));
    // A foreign or unreadable metadata slot is reserved without being loaded.
    EXPECT_EQ(Vulkan::NextUnusedStoredSlot(0, 4), 5u);
    EXPECT_EQ(Vulkan::NextUnusedStoredSlot(5, 1), 5u);
    EXPECT_EQ(Vulkan::NextUnusedStoredSlot(5, 8), 9u);
    const auto sparse = [](size_t index) { return index == 9 || index == 10 || index == 25; };
    EXPECT_EQ(Vulkan::FindUnusedStoredSlot(9, sparse), 11u);
    EXPECT_EQ(Vulkan::FindUnusedStoredSlot(25, sparse), 26u);
}
