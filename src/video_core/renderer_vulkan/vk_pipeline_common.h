// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <string_view>
#include <thread>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
struct PushData;
} // namespace Shader

namespace Vulkan {

/// FEAT-SGT-022 step 2: how many recorder threads keep their own draw stages in a pipeline, and
/// the slot this thread installs and reads them in. 0 on the one recorder today and on every
/// thread that records nothing (those read what slot 0 holds, as they read `stages` before).
inline constexpr u32 MaxRecorders = 2;
inline constinit thread_local u32 t_recorder_slot = 0;

/// FIX-GT7-101: a pipeline (or library) the driver failed to create is created again after a
/// pause (0.2 s, doubling, six times) instead of ending the game at once. On a first GT7 race
/// load up to 21 large compiles ran at once and the process committed 9.6 GB; players reported
/// crashes during race loads, three tries before one got through (the cache kept what each try
/// compiled). The failure and its code are logged; a seventh failure is left to the caller's check.
template <typename Create>
auto CreateWithRetries(const char* what, std::string_view name, Create&& create) {
    auto result = create();
    for (u32 attempt = 1; attempt <= 6 && result.result != vk::Result::eSuccess; ++attempt) {
        const u32 pause_ms = 100u << attempt;
        LOG_ERROR(Render_Vulkan, "FIX-GT7-101: {} {} failed ({}), trying again in {} ms ({} of 6)",
                  what, name, vk::to_string(result.result), pause_ms, attempt);
        std::this_thread::sleep_for(std::chrono::milliseconds{pause_ms});
        result = create();
    }
    return result;
}

static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Instance;
class Scheduler;
class DescriptorHeap;

class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
             bool is_compute = false);
    virtual ~Pipeline();

    void NoteRetentionUse(std::chrono::steady_clock::time_point now) const {
        retention_use.store(now.time_since_epoch().count(), std::memory_order_relaxed);
    }

    std::chrono::steady_clock::time_point LastRetentionUse() const {
        return std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{
            retention_use.load(std::memory_order_relaxed)}};
    }

    vk::Pipeline Handle() const noexcept {
        // PERF-017: the fully optimized pipeline once a worker has linked it.
        if (const VkPipeline optimized = optimized_pipeline.load(std::memory_order_acquire)) {
            return optimized;
        }
        return *pipeline;
    }

    vk::PipelineLayout GetLayout() const noexcept {
        return *pipeline_layout;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::SwStage::Compute) == Shader::MaxStageTypes - 1);
        const auto& stages = draw_stages[t_recorder_slot].infos;
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::SwStage stage) const noexcept {
        return *draw_stages[t_recorder_slot].infos[u32(stage)];
    }

    /// PERF-047: the pipeline cache's infos the pipeline was made from (stages may point at the
    /// GPU recorder's copies of them while a draw is recorded).
    const std::array<const Shader::Info*, Shader::MaxStageTypes>& CanonicalStages() const {
        return canonical_stages;
    }
    /// PERF-047: the infos the draw being recorded binds through (a recorder thread, in its
    /// slot; FEAT-SGT-022).
    void SetDrawStages(const std::array<const Shader::Info*, Shader::MaxStageTypes>& infos) const {
        draw_stages[t_recorder_slot].infos = infos;
    }

    bool IsCompute() const {
        return is_compute;
    }

    /// NAT-100 / SHD-002: how the pipeline takes its descriptors (native mode binds them
    /// itself): one set per stage (Shader::Backend::StageSet), one of them pushed.
    static constexpr u32 NoPushSet = ~0u;
    u32 PushSet() const {
        return push_set;
    }
    vk::DescriptorSetLayout SetLayout(u32 set) const {
        return set < set_layouts.size() ? set_layouts[set] : vk::DescriptorSetLayout{};
    }
    /// PERF-GTS-374: how many of the set's bindings (its first ones, the stage's buffers) are
    /// storage buffers with dynamic offsets; 0 when the set binds them at fixed offsets.
    u32 DynamicBuffers(u32 set) const {
        return set < set_dynamic_buffers.size() ? set_dynamic_buffers[set] : 0;
    }

    using DescriptorWrites = std::vector<vk::WriteDescriptorSet>;
    /// SHD-002: the descriptor set of each write.
    using WriteSets = std::vector<u8>;
    void BindResources(DescriptorWrites& set_writes, const WriteSets& write_sets,
                       const Shader::PushData& push_data) const;

protected:
    mutable std::atomic<std::chrono::steady_clock::duration::rep> retention_use{0};
    [[nodiscard]] std::string GetDebugString() const;

    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    vk::UniquePipeline pipeline;
    std::atomic<VkPipeline> optimized_pipeline{};
    vk::UniquePipelineLayout pipeline_layout;
    /// SHD-002: the layouts of the stage sets (set_layouts also holds the empty layout of sets
    /// between them).
    std::array<vk::UniqueDescriptorSetLayout, Shader::Backend::MaxDescriptorSets> desc_layouts;
    std::array<vk::DescriptorSetLayout, Shader::Backend::MaxDescriptorSets> set_layouts{};
    /// The pipeline cache's info of a stage, in every recorder's slot until a draw installs its own.
    void SetCanonicalStage(u32 stage, const Shader::Info* info) {
        canonical_stages[stage] = info;
        for (auto& slot : draw_stages) {
            slot.infos[stage] = info;
        }
    }
    /// FEAT-SGT-022 step 2: one line each, so two recorders installing never share one.
    struct alignas(64) DrawStages {
        std::array<const Shader::Info*, Shader::MaxStageTypes> infos{};
    };
    mutable std::array<DrawStages, MaxRecorders> draw_stages{};
    std::array<const Shader::Info*, Shader::MaxStageTypes> canonical_stages{};
    u32 push_set = NoPushSet;
    std::array<u8, Shader::Backend::MaxDescriptorSets> set_dynamic_buffers{}; ///< PERF-GTS-374
    bool is_compute;
};

} // namespace Vulkan
