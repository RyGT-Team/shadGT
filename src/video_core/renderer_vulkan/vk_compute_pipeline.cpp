// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <atomic>
#include <chrono>
#include <boost/container/small_vector.hpp>

#include "common/perf_monitor.h"
#include "common/scope_exit.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/lane_read_policy.h"
#include "video_core/renderer_vulkan/compute_warm_recipe.h"
#include "video_core/renderer_vulkan/compiler_worker_budget.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {
/// The subgroup size compute pipelines are built with on this device, and whether it is asked
/// for (FIX-GT7-026: the same rule for a pipeline and for a recipe built ahead).
u32 RequiredSubgroup(const Instance& instance) {
    return instance.IsSubgroupSize64Supported() ? 64u : 32u;
}

bool UseSubgroupControl(const Instance& instance) {
    // FIX-188: Intel Arc (#65) runs compute in 32-wide subgroups when it can't do 64.
    return instance.IsSubgroupSize64Supported() ||
           (instance.IsIntel() && instance.IsSubgroupSizeControlSupported() &&
            instance.SubgroupSize() == 32u);
}
} // namespace

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, const Shader::Profile& profile,
                                 vk::PipelineCache pipeline_cache, ComputePipelineKey compute_key_,
                                 const Shader::Info& info_, vk::ShaderModule module,
                                 SerializationSupport& sdata)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      compute_key{compute_key_} {
    auto& info = stages[int(Shader::SwStage::Compute)];
    info = &info_;
    canonical_stages[int(Shader::SwStage::Compute)] = &info_;
    const auto debug_str = GetDebugString();

    const u32 required_subgroup = RequiredSubgroup(instance);
    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = required_subgroup,
    };
    const bool use_subgroup_control = UseSubgroupControl(instance);
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .pNext = use_subgroup_control ? &subgroup_size_ci : nullptr,
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };

    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info->buffers) {
        const auto sharp = buffer.GetSharp(*info);
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (const auto& image : info->images) {
        const u32 num_bindings = image.NumBindings(*info);
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    for (const auto& sampler : info->samplers) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    const bool push = binding < instance.MaxPushDescriptors();
    push_set = push ? 0u : NoPushSet;
    recipe_bindings.assign(bindings.begin(), bindings.end()); // PERF-GT7-037
    recipe_push = push;
    recipe_subgroup_control = use_subgroup_control;
    recipe_subgroup = required_subgroup;
    const auto flags = push ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                            : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    const auto device = instance.GetDevice();
    auto [descriptor_set_result, descriptor_set] =
        device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,
               "Failed to create compute descriptor set layout: {}",
               vk::to_string(descriptor_set_result));
    desc_layouts[0] = std::move(descriptor_set);
    set_layouts[0] = *desc_layouts[0];

    const vk::DescriptorSetLayout set_layout = set_layouts[0];
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create compute pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Compute PipelineLayout {}", debug_str);

    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pipeline_layout,
    };
    auto [pipeline_result, pipe] = CreateWithRetries("compute pipeline", debug_str, [&] {
        return instance.GetDevice().createComputePipelineUnique(pipeline_cache,
                                                                compute_pipeline_ci);
    }); // FIX-GT7-101
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Compute Pipeline {}", debug_str);
}

ComputePipeline::~ComputePipeline() = default;

namespace {
constexpr u32 RecipeMagic = 0x43574752; // "RGWC"
constexpr u32 RecipeVersion = 2;
u64 RecipeSignature(const Instance& instance, const Shader::Profile& p) {
    u64 hash = 1469598103934665603ull;
    const auto add = [&](u64 v) { for (u32 i=0;i<8;++i) { hash ^= (v >> (i*8)) & 255; hash *= 1099511628211ull; } };
    const auto props = instance.GetPhysicalDevice().getProperties();
    // Recipe translation ABI: bump when stored SPIR-V layout or policy changes.
    constexpr u32 TranslationAbiVersion = 3; // FIX-317: six push-offset vectors.
    add(TranslationAbiVersion);
    add(Shader::LaneReadUnbranchEnabled());
    add(props.vendorID);
    for (const auto v : {u64(p.direct_buffers),u64(p.direct_min_bytes),u64(p.max_viewport_width),u64(p.max_viewport_height),
        u64(p.max_shared_memory_size),u64(p.supported_spirv),u64(p.subgroup_size),u64(p.sparse_page_shift),
        u64(p.support_int8),u64(p.support_int16),u64(p.support_int64),u64(p.support_float16),u64(p.support_float64),
        u64(p.supports_denorm_behavior_independence),u64(p.supports_rounding_mode_independence),
        u64(p.support_fp16_denorm_preserve),u64(p.support_fp16_denorm_flush),u64(p.support_fp16_round_to_zero),
        u64(p.support_fp32_denorm_preserve),u64(p.discard_kills),u64(p.support_fp32_denorm_flush),
        u64(p.support_fp32_round_to_zero),u64(p.support_fp64_denorm_preserve),u64(p.support_fp64_denorm_flush),
        u64(p.support_fp64_round_to_zero),u64(p.support_fp16_signed_zero_inf_nan_preserve),
        u64(p.support_fp32_signed_zero_inf_nan_preserve),u64(p.support_fp64_signed_zero_inf_nan_preserve),
        u64(p.supports_image_load_store_lod),u64(p.supports_native_cube_calc),u64(p.native_cube_sampling),
        u64(p.supports_trinary_minmax),u64(p.supports_buffer_fp32_atomic_min_max),u64(p.supports_image_fp32_atomic_min_max),
        u64(p.supports_buffer_int64_atomics),u64(p.supports_shared_int64_atomics),u64(p.supports_workgroup_explicit_memory_layout),
        u64(p.supports_amd_shader_explicit_vertex_parameter),u64(p.supports_fragment_shader_barycentric),
        u64(p.supports_shader_subgroup_clock),u64(p.amd_vertex_order),u64(p.amd_nan_rules),
        u64(p.supports_quad_derivatives),u64(p.vertex_order_check),u64(p.has_broken_spirv_clamp),
        u64(p.lower_left_origin_mode),u64(p.needs_manual_interpolation),u64(p.needs_lds_barriers),
        u64(p.intel_arc_workarounds),u64(p.needs_buffer_offsets),u64(p.needs_unorm_fixup),
        u64(p.needs_clip_distance_emulation),u64(p.supports_shader_stencil_export)}) add(v);
    const std::array<const char*, 4> policies{std::getenv("SHADGT_DISABLE_PERF"),
        std::getenv("RYGT_FMASK_KEY"), std::getenv("RYGT_UNNORMALIZED_MIP"),
        std::getenv("RYGT_FLOAT_ATOMIC_EXACT")};
    for (const char* str : policies) {
        if (str) for (; *str; ++str) add(static_cast<unsigned char>(*str));
        add(0xff);
    }
    return hash;
}
} // namespace

u64 ComputePipeline::RecipePolicy(const Instance& instance, const Shader::Profile& profile) {
    return RecipeSignature(instance, profile);
}

bool ComputePipeline::SaveRecipe(const std::filesystem::path& file,
                                 std::span<const u32> spirv) const {
    const u64 signature = RecipeSignature(instance, profile);
    std::vector<u32> words{RecipeMagic,
                           RecipeVersion,
                           u32(signature), u32(signature >> 32),
                           u32(recipe_push),
                           u32(recipe_subgroup_control),
                           recipe_subgroup,
                           u32(recipe_bindings.size())};
    for (const auto& b : recipe_bindings) {
        words.push_back(b.binding);
        words.push_back(u32(b.descriptorType));
        words.push_back(b.descriptorCount);
    }
    words.push_back(u32(spirv.size()));
    words.insert(words.end(), spirv.begin(), spirv.end());
    return WriteComputeWarmRecipeExclusive(file, words);
}

bool ComputePipeline::WarmFromRecipe(const Instance& instance, const Shader::Profile& profile, vk::PipelineCache cache,
                                     const std::filesystem::path& file, std::stop_token stop) {
    std::ifstream in{file, std::ios::binary};
    std::vector<u32> words;
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        if (ec || size < 9 * sizeof(u32) || size % sizeof(u32) != 0 || size > (16u << 20)) {
            return false;
        }
        // Skip stale policies before allocating/reading an entire old recipe.
        std::array<u32, 4> header{};
        in.read(reinterpret_cast<char*>(header.data()), sizeof(header));
        if (!in || header[0] != RecipeMagic || header[1] != RecipeVersion ||
            (u64(header[3]) << 32 | header[2]) != RecipeSignature(instance, profile)) return false;
        in.seekg(0);
        words.resize(size / sizeof(u32));
        in.read(reinterpret_cast<char*>(words.data()), std::streamsize(size));
        if (!in) {
            return false;
        }
    }
    if (!ValidateComputeWarmRecipe(words, RecipeSignature(instance, profile),
        instance.MaxPushDescriptors(), profile.supported_spirv,
        instance.IsIntel() && instance.IsSubgroupSizeControlSupported() && instance.SubgroupSize() == 32,
        instance.IsSubgroupSize64Supported())) return false;
    const bool push = words[4] != 0;
    const bool subgroup_control = words[5] != 0;
    const u32 subgroup = words[6];
    const u32 count = words[7];
    size_t at = 8;
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    for (u32 i = 0; i < count; ++i, at += 3) {
        bindings.push_back({
            .binding = words[at],
            .descriptorType = vk::DescriptorType(words[at + 1]),
            .descriptorCount = words[at + 2],
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    // FIX-GT7-026: a recipe goes to the driver only as this device would build its pipeline:
    // the same subgroup size request, and push descriptors exactly when the constructor would
    // use them. Another one is skipped, never left to the driver to refuse.
    if (subgroup_control != UseSubgroupControl(instance) ||
        (subgroup_control && subgroup != RequiredSubgroup(instance))) {
        return false;
    }
    u32 next_binding = 0;
    for (const auto& b : bindings) {
        next_binding += b.descriptorCount;
    }
    if (push != (next_binding < instance.MaxPushDescriptors())) {
        return false;
    }
    const u32 spirv_words = words[at++];
    const auto device = instance.GetDevice();
    auto [module_result, module] = device.createShaderModuleUnique({
        .codeSize = spirv_words * sizeof(u32),
        .pCode = words.data() + at,
    });
    if (module_result != vk::Result::eSuccess) {
        return false;
    }
    const vk::DescriptorSetLayoutCreateInfo set_ci = {
        .flags = push ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                      : vk::DescriptorSetLayoutCreateFlagBits{},
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto [set_result, set_layout] = device.createDescriptorSetLayoutUnique(set_ci);
    if (set_result != vk::Result::eSuccess) {
        return false;
    }
    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };
    const vk::DescriptorSetLayout set = *set_layout;
    auto [layout_result, layout] = device.createPipelineLayoutUnique({
        .setLayoutCount = 1U,
        .pSetLayouts = &set,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    });
    if (layout_result != vk::Result::eSuccess) {
        return false;
    }
    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_ci = {
        .requiredSubgroupSize = subgroup,
    };
    const vk::ComputePipelineCreateInfo pipeline_ci = {
        .stage =
            {
                .pNext = subgroup_control ? &subgroup_ci : nullptr,
                .stage = vk::ShaderStageFlagBits::eCompute,
                .module = *module,
                .pName = "main",
            },
        .layout = *layout,
    };
    const bool gate = Common::PerfFeatureEnabled(79);
    if (gate) {
        while (!HeavyPipelineGate().try_acquire_for(std::chrono::milliseconds{50}))
            if (stop.stop_requested()) return false;
    }
    SCOPE_EXIT { if (gate) HeavyPipelineGate().release(); };
    if (stop.stop_requested()) return false;
    auto [pipeline_result, pipeline] = device.createComputePipelineUnique(cache, pipeline_ci);
    return pipeline_result == vk::Result::eSuccess;
}

} // namespace Vulkan
