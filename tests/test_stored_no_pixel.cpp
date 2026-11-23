// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// FIX-SGT-116: which stored pipelines without a pixel shader the startup precompile takes.

#include <vector>
#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/stored_no_pixel.h"

namespace {

// What a loaded record holds: the discard shader is never stored, so there is no field for it.
struct BuildData {
    struct {
        unsigned sampleShadingEnable{};
        unsigned alphaToCoverageEnable{};
    } multisampling;
    std::vector<unsigned> tcs;
    std::vector<unsigned> tes;
};

Shader::RuntimeInfo VertexStage() {
    Shader::RuntimeInfo info{};
    info.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
    info.hw.vs.outputs[0] = {Shader::Output::PointSize, Shader::Output::None, Shader::Output::None,
                             Shader::Output::None};
    return info;
}

// The reader's test and the installer's, as the precompile applies them (NVIDIA unless said).
bool Taken(const BuildData& data, const Shader::RuntimeInfo& vertex, bool emulates = true) {
    return Vulkan::StoredBuildIgnoresPixelInfo(data, false, false, false) &&
           !Vulkan::VertexStageMayClip(vertex, emulates);
}

TEST(StoredNoPixel, TakesABuildThatReadNothingOfThePixelStage) {
    EXPECT_TRUE(Taken(BuildData{}, VertexStage()));
}

// The defect of the first version: a record with clip distances looked clean, as every loaded one
// does, and was rebuilt without the discard shader a draw may have had.
TEST(StoredNoPixel, RefusesClipDistancesTheRecordCannotShow) {
    for (auto clip = static_cast<int>(Shader::Output::ClipDist0);
         clip <= static_cast<int>(Shader::Output::ClipDist7); ++clip) {
        auto vertex = VertexStage();
        vertex.hw.vs.outputs[clip % 3][clip % 4] = static_cast<Shader::Output>(clip);
        EXPECT_FALSE(Taken(BuildData{}, vertex)) << clip;
    }
}

TEST(StoredNoPixel, CullDistancesDoNotClip) {
    auto vertex = VertexStage();
    vertex.hw.vs.outputs[1] = {Shader::Output::CullDist0, Shader::Output::CullDist7,
                               Shader::Output::None, Shader::Output::None};
    EXPECT_TRUE(Taken(BuildData{}, vertex));
}

TEST(StoredNoPixel, ClipDistancesWithoutTheEmulationAreTaken) {
    auto vertex = VertexStage();
    vertex.hw.vs.outputs[2][3] = Shader::Output::ClipDist7;
    EXPECT_TRUE(Taken(BuildData{}, vertex, false));
}

TEST(StoredNoPixel, RefusesAVertexStageThatIsNotTheHardwareOne) {
    Shader::RuntimeInfo exported{};
    exported.Initialize(Shader::HwStage::Export, Shader::SwStage::Vertex);
    EXPECT_FALSE(Taken(BuildData{}, exported));
    EXPECT_TRUE(Taken(BuildData{}, exported, false));
}

TEST(StoredNoPixel, RefusesSampleShading) {
    BuildData data;
    data.multisampling.sampleShadingEnable = 1;
    EXPECT_FALSE(Taken(data, VertexStage()));
}

TEST(StoredNoPixel, RefusesHelperControlShaders) {
    BuildData data;
    data.tcs = {0x07230203};
    EXPECT_FALSE(Taken(data, VertexStage()));
}

TEST(StoredNoPixel, RefusesHelperEvaluationShaders) {
    BuildData data;
    data.tes = {0x07230203};
    EXPECT_FALSE(Taken(data, VertexStage()));
}

TEST(StoredNoPixel, RefusesRectAndQuadLists) {
    EXPECT_FALSE(Vulkan::StoredBuildIgnoresPixelInfo(BuildData{}, true, false, false));
}

TEST(StoredNoPixel, RefusesTessellation) {
    EXPECT_FALSE(Vulkan::StoredBuildIgnoresPixelInfo(BuildData{}, false, true, false));
}

TEST(StoredNoPixel, RefusesMultisampledWithShaderObjects) {
    EXPECT_FALSE(Vulkan::StoredBuildIgnoresPixelInfo(BuildData{}, false, false, true));
}

} // namespace
