// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <span>
#include <stop_token>
#include <vector>
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Serialization {
struct Archive;
}

namespace Vulkan {

class Instance;
class Scheduler;
class DescriptorHeap;

struct ComputePipelineKey {
    size_t value;

    friend bool operator==(const ComputePipelineKey& lhs, const ComputePipelineKey& rhs) {
        return lhs.value == rhs.value;
    }
    friend bool operator!=(const ComputePipelineKey& lhs, const ComputePipelineKey& rhs) {
        return !(lhs == rhs);
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

class ComputePipeline : public Pipeline {
public:
    struct SerializationSupport {
        u32 dummy{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    ComputePipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                    const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
                    ComputePipelineKey compute_key, const Shader::Info& info,
                    vk::ShaderModule module, SerializationSupport& sdata);
    ~ComputePipeline();

    /// PERF-GT7-037: what builds this pipeline again exactly (its SPIR-V, set layout, push
    /// range and subgroup size), written to `file`. WarmFromRecipe builds it from that file and
    /// drops it: the driver's caches keep it, and a later build of the same pipeline is quick.
    bool SaveRecipe(const std::filesystem::path& file, std::span<const u32> spirv) const;
    static u64 RecipePolicy(const Instance& instance, const Shader::Profile& profile);
    static bool WarmFromRecipe(const Instance& instance, const Shader::Profile& profile, vk::PipelineCache cache,
                               const std::filesystem::path& file, std::stop_token stop);
    /// The same, from a recipe's words.
    static bool WarmFromWords(const Instance& instance, const Shader::Profile& profile, vk::PipelineCache cache,
                              std::span<const u32> words, std::stop_token stop);
    /// FEAT-SGT-118: the set's bindings of a compute shader; the constructor's rule.
    static u32 StageBindings(const Shader::Info& info,
                             std::vector<vk::DescriptorSetLayoutBinding>& bindings);
    /// FEAT-SGT-118: a translation's recipe words, before any dispatch. A mip fallback indexed at
    /// runtime takes its count from the T# in `info` (the recorded one, for a replayed translation):
    /// another count at the dispatch only means that this build served nothing.
    static std::vector<u32> AheadRecipe(const Instance& instance, const Shader::Profile& profile,
                                        const Shader::Info& info, std::span<const u32> spirv);

    void SetStageInfo(const Shader::Info& info) {
        SetCanonicalStage(u32(Shader::SwStage::Compute), &info);
    }

private:
    ComputePipelineKey compute_key;
    std::vector<vk::DescriptorSetLayoutBinding> recipe_bindings; ///< PERF-GT7-037
    bool recipe_push = false;
    bool recipe_subgroup_control = false;
    u32 recipe_subgroup = 0;
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::ComputePipelineKey> {
    std::size_t operator()(const Vulkan::ComputePipelineKey& key) const noexcept {
        return std::hash<size_t>{}(key.value);
    }
};
