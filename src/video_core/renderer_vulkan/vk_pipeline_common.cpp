// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include "video_core/renderer_vulkan/descriptor_write_groups.h"
#include "ryty_gpu/native/descriptor_update_batch.h"

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;

void Pipeline::BindResources(DescriptorWrites& set_writes, const WriteSets& write_sets,
                             const Shader::PushData& push_data) const {
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;

    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    cmdbuf.pushConstants(*pipeline_layout, stage_flags, 0u, sizeof(push_data), &push_data);

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    // SHD-002: each stage's writes go to its own set.
    static thread_local std::array<boost::container::small_vector<vk::WriteDescriptorSet, 32>,
               Shader::Backend::MaxDescriptorSets>
        scratch;
    const auto by_set = GroupDescriptorWrites<vk::WriteDescriptorSet>(
        {set_writes.data(),set_writes.size()},{write_sets.data(),write_sets.size()},scratch);
    static thread_local boost::container::small_vector<vk::WriteDescriptorSet,64> updates;
    updates.clear();
    std::array<vk::DescriptorSet,Shader::Backend::MaxDescriptorSets> sets{};
    for (u32 set = 0; set < by_set.size(); ++set) {
        auto& writes = by_set[set];
        if (writes.empty()) {
            continue;
        }
        if (set == push_set) {
            continue;
        }
        const auto desc_set = desc_heap.Commit(set_layouts[set]);
        sets[set] = desc_set;
        Native::AppendDescriptorUpdates(writes,desc_set,updates);
    }
    Native::CommitDescriptorBatch(updates,sets,push_set,by_set,
        [&](const auto& all){instance.GetDevice().updateDescriptorSets(all,{});},
        [&](u32 set,const auto& writes){cmdbuf.pushDescriptorSetKHR(bind_point,*pipeline_layout,set,writes);},
        [&](u32 set,vk::DescriptorSet value){cmdbuf.bindDescriptorSets(bind_point,*pipeline_layout,set,value,{});});
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
