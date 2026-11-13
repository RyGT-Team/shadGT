// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later

// DIAG-SHC-001: see pipeline_compile_log.h. Everything here runs only with the switch set.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstring>
#include <type_traits>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/types.h"
#include <xxhash.h>
#include "common/path_util.h"
#include "video_core/renderer_vulkan/pipeline_compile_log.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Vulkan::PipelineCompileLog {
namespace {

PFN_vkCreateGraphicsPipelines real_create_graphics{};
PFN_vkCreateComputePipelines real_create_compute{};
PFN_vkCreateShadersEXT real_create_shaders{};
PFN_vkCreateShaderModule real_create_module{};
PFN_vkDestroyPipeline destroy_pipeline{};
PFN_vkSetDebugUtilsObjectNameEXT real_set_name{};
PFN_vkCmdBindPipeline real_bind{};
PFN_vkCmdInsertDebugUtilsLabelEXT insert_label{};
PFN_vkGetPipelineExecutablePropertiesKHR get_executables{};
PFN_vkGetPipelineExecutableStatisticsKHR get_statistics{};
bool capture_statistics = false;

const auto start_time = std::chrono::steady_clock::now();
std::mutex file_mutex;
std::FILE* file = nullptr;

// Pipelines built but not yet bound: the label the first bind inserts.
std::mutex pending_mutex;
std::unordered_map<VkPipeline, std::string> pending;
std::atomic<size_t> pending_count{0};

double NowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_time)
        .count();
}

unsigned long ThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return 0;
#endif
}

void WriteRow(const std::string& row) {
    std::scoped_lock lock{file_mutex};
    if (!file) return;
    std::fputs(row.c_str(), file);
    std::fputc('\n', file);
    std::fflush(file);
}

std::string Quote(std::string text) {
    for (char& c : text) {
        if (c == '"' || c == '\n' || c == '\r') c = '\'';
    }
    return '"' + text + '"';
}

const VkBaseInStructure* FindInChain(const void* next, VkStructureType type) {
    for (auto* s = static_cast<const VkBaseInStructure*>(next); s; s = s->pNext) {
        if (s->sType == type) return s;
    }
    return nullptr;
}

const char* StageName(VkShaderStageFlagBits stage) {
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT: return "vs";
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return "hs";
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return "ds";
    case VK_SHADER_STAGE_GEOMETRY_BIT: return "gs";
    case VK_SHADER_STAGE_FRAGMENT_BIT: return "ps";
    case VK_SHADER_STAGE_COMPUTE_BIT: return "cs";
    default: return "other";
    }
}

// amdvlk can fault inside these queries for some pipelines; a fault turns statistics off
// for the rest of the run instead of taking the game down.
std::atomic<bool> statistics_faulted{false};

VkResult SafeExecutables(VkDevice device, const VkPipelineInfoKHR* info, u32* count,
                         VkPipelineExecutablePropertiesKHR* out) {
#ifdef _WIN32
    __try {
        return get_executables(device, info, count, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        statistics_faulted.store(true);
        return VK_ERROR_UNKNOWN;
    }
#else
    return get_executables(device, info, count, out);
#endif
}

VkResult SafeStatistics(VkDevice device, const VkPipelineExecutableInfoKHR* info, u32* count,
                        VkPipelineExecutableStatisticKHR* out) {
#ifdef _WIN32
    __try {
        return get_statistics(device, info, count, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        statistics_faulted.store(true);
        return VK_ERROR_UNKNOWN;
    }
#else
    return get_statistics(device, info, count, out);
#endif
}
std::string Statistics(VkDevice device, VkPipeline pipeline, bool captured) {
    // amdvlk crashes when asked about a pipeline built without the capture flag.
    if (!captured || !capture_statistics || statistics_faulted.load() || !get_executables || !get_statistics || !pipeline) return {};
    const VkPipelineInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, nullptr, pipeline};
    u32 count = 0;
    if (SafeExecutables(device, &info, &count, nullptr) != VK_SUCCESS || count == 0) return {};
    std::vector<VkPipelineExecutablePropertiesKHR> executables(
        count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    if (SafeExecutables(device, &info, &count, executables.data()) != VK_SUCCESS) return {};
    std::string out;
    for (u32 i = 0; i < count; ++i) {
        const VkPipelineExecutableInfoKHR exe{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,
                                              nullptr, pipeline, i};
        u32 stats = 0;
        if (SafeStatistics(device, &exe, &stats, nullptr) != VK_SUCCESS || stats == 0) continue;
        std::vector<VkPipelineExecutableStatisticKHR> values(
            stats, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        if (SafeStatistics(device, &exe, &stats, values.data()) != VK_SUCCESS) continue;
        out += fmt::format("{}[", executables[i].name);
        for (u32 j = 0; j < stats; ++j) {
            const auto& v = values[j];
            std::string value;
            switch (v.format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: value = v.value.b32 ? "1" : "0"; break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: value = std::to_string(v.value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: value = std::to_string(v.value.u64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: value = fmt::format("{:.3g}", v.value.f64); break;
            default: break;
            }
            out += fmt::format("{}{}={}", j ? ";" : "", v.name, value);
        }
        out += "] ";
    }
    return out;
}

// SPIR-V hash of each shader module, to tell whether a pipeline that misses the driver cache
// again in the next run was handed different code.
std::mutex module_mutex;
std::unordered_map<VkShaderModule, u64> module_hashes;

// Level 4: each distinct SPIR-V is written once to logs/spirv/<hash>.spv, for offline compiles
// (RGA) of the shaders the CSV shows as slow.
std::unordered_set<u64> dumped_spirv;
void DumpSpirv(u64 hash, const u32* code, size_t size) {
    if (Level() < 4) return;
    {
        std::scoped_lock lock{module_mutex};
        if (!dumped_spirv.insert(hash).second) return;
    }
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "spirv";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / fmt::format("{:016x}.spv", hash), std::ios::binary);
    out.write(reinterpret_cast<const char*>(code), static_cast<std::streamsize>(size));
}

VKAPI_ATTR VkResult VKAPI_CALL CreateModule(VkDevice device, const VkShaderModuleCreateInfo* info,
                                            const VkAllocationCallbacks* alloc,
                                            VkShaderModule* module) {
    const VkResult result = real_create_module(device, info, alloc, module);
    if (result == VK_SUCCESS && info && info->pCode) {
        const u64 hash = XXH3_64bits(info->pCode, info->codeSize);
        {
            std::scoped_lock lock{module_mutex};
            module_hashes[*module] = hash;
        }
        DumpSpirv(hash, info->pCode, info->codeSize);
    }
    return result;
}

// "vs=<spirv hash>[+<specialization hash>];ps=..." for the stages handed to the driver.
std::string StageHashes(const VkPipelineShaderStageCreateInfo* infos, u32 count) {
    std::string out;
    for (u32 i = 0; infos && i < count; ++i) {
        const auto& s = infos[i];
        u64 hash = 0;
        if (s.module) {
            std::scoped_lock lock{module_mutex};
            if (auto it = module_hashes.find(s.module); it != module_hashes.end()) hash = it->second;
        } else if (auto* inline_code = reinterpret_cast<const VkShaderModuleCreateInfo*>(
                       FindInChain(s.pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO));
                   inline_code && inline_code->pCode) {
            hash = XXH3_64bits(inline_code->pCode, inline_code->codeSize);
            DumpSpirv(hash, inline_code->pCode, inline_code->codeSize);
        }
        out += fmt::format("{}{}={:016x}", i ? ";" : "", StageName(s.stage), hash);
        if (s.pSpecializationInfo && s.pSpecializationInfo->dataSize) {
            out += fmt::format("+{:08x}", static_cast<u32>(XXH3_64bits(
                                              s.pSpecializationInfo->pData,
                                              s.pSpecializationInfo->dataSize)));
        }
    }
    return out;
}

// Value hashes of samplers, set layouts and pipeline layouts, so a layout rebuilt in another run
// with the same contents gets the same hash (handles change from run to run, contents should not).
PFN_vkCreateSampler real_create_sampler{};
PFN_vkCreateDescriptorSetLayout real_create_set_layout{};
PFN_vkCreatePipelineLayout real_create_layout{};
std::mutex object_mutex;
std::unordered_map<u64, u64> object_hashes; // handle -> content hash

struct Hasher {
    std::vector<u8> bytes;
    template <typename T>
    void Add(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const u8*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
    void AddBytes(const void* data, size_t size) {
        if (!data) return Add(size_t{0});
        const auto* p = static_cast<const u8*>(data);
        bytes.insert(bytes.end(), p, p + size);
    }
    u64 Done() const {
        return XXH3_64bits(bytes.data(), bytes.size());
    }
};

u64 ObjectHash(u64 handle) {
    if (!handle) return 0;
    std::scoped_lock lock{object_mutex};
    const auto it = object_hashes.find(handle);
    return it != object_hashes.end() ? it->second : 0xdeadull;
}

void NoteObject(u64 handle, u64 hash) {
    std::scoped_lock lock{object_mutex};
    object_hashes[handle] = hash;
}

// sType/pNext chains: hash the sTypes so an added or dropped extension struct shows.
void AddChainTypes(Hasher& h, const void* next) {
    for (auto* s = static_cast<const VkBaseInStructure*>(next); s; s = s->pNext) h.Add(s->sType);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(VkDevice device, const VkSamplerCreateInfo* info,
                                             const VkAllocationCallbacks* alloc, VkSampler* out) {
    const VkResult result = real_create_sampler(device, info, alloc, out);
    if (result == VK_SUCCESS && info) {
        Hasher h;
        h.AddBytes(reinterpret_cast<const u8*>(info) + 16, sizeof(*info) - 16); // after sType/pNext
        AddChainTypes(h, info->pNext);
        if (auto* border = reinterpret_cast<const VkSamplerCustomBorderColorCreateInfoEXT*>(
                FindInChain(info->pNext, VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT))) {
            h.Add(border->customBorderColor);
            h.Add(border->format);
        }
        NoteObject(reinterpret_cast<u64>(*out), h.Done());
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSetLayout(VkDevice device,
                                               const VkDescriptorSetLayoutCreateInfo* info,
                                               const VkAllocationCallbacks* alloc,
                                               VkDescriptorSetLayout* out) {
    const VkResult result = real_create_set_layout(device, info, alloc, out);
    if (result == VK_SUCCESS && info) {
        Hasher h;
        h.Add(info->flags);
        AddChainTypes(h, info->pNext);
        for (u32 i = 0; i < info->bindingCount; ++i) {
            const auto& b = info->pBindings[i];
            h.Add(b.binding);
            h.Add(b.descriptorType);
            h.Add(b.descriptorCount);
            h.Add(b.stageFlags);
            for (u32 j = 0; b.pImmutableSamplers && j < b.descriptorCount; ++j) {
                h.Add(ObjectHash(reinterpret_cast<u64>(b.pImmutableSamplers[j])));
            }
        }
        NoteObject(reinterpret_cast<u64>(*out), h.Done());
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateLayout(VkDevice device, const VkPipelineLayoutCreateInfo* info,
                                            const VkAllocationCallbacks* alloc,
                                            VkPipelineLayout* out) {
    const VkResult result = real_create_layout(device, info, alloc, out);
    if (result == VK_SUCCESS && info) {
        Hasher h;
        h.Add(info->flags);
        for (u32 i = 0; i < info->setLayoutCount; ++i) {
            h.Add(ObjectHash(reinterpret_cast<u64>(info->pSetLayouts[i])));
        }
        for (u32 i = 0; i < info->pushConstantRangeCount; ++i) h.Add(info->pPushConstantRanges[i]);
        NoteObject(reinterpret_cast<u64>(*out), h.Done());
    }
    return result;
}

template <typename T>
u32 Short(const T* p) {
    if (!p) return 0;
    Hasher h;
    h.AddBytes(reinterpret_cast<const u8*>(p) + 16, sizeof(T) - 16); // fields after sType/pNext
    AddChainTypes(h, p->pNext);
    return static_cast<u32>(h.Done());
}

// "flags=..;vi=..;ia=..;ts=..;vp=..;rs=..;ms=..;ds=..;cb=..;dyn=..;rend=..;chain=..;layout=..":
// one short hash per part of a whole graphics pipeline's description, pointers left out.
std::string StateHashes(const VkGraphicsPipelineCreateInfo& ci) {
    auto part = [](u64 v) { return fmt::format("{:08x}", static_cast<u32>(v)); };
    std::string out = fmt::format("flags={:x}", ci.flags);
    if (auto* vi = ci.pVertexInputState) {
        Hasher h;
        h.Add(vi->flags);
        for (u32 i = 0; i < vi->vertexBindingDescriptionCount; ++i) h.Add(vi->pVertexBindingDescriptions[i]);
        for (u32 i = 0; i < vi->vertexAttributeDescriptionCount; ++i) h.Add(vi->pVertexAttributeDescriptions[i]);
        AddChainTypes(h, vi->pNext);
        out += ";vi=" + part(h.Done());
    }
    out += ";ia=" + part(Short(ci.pInputAssemblyState));
    out += ";ts=" + part(Short(ci.pTessellationState));
    if (auto* vp = ci.pViewportState) {
        Hasher h;
        h.Add(vp->viewportCount);
        h.Add(vp->scissorCount);
        AddChainTypes(h, vp->pNext);
        out += ";vp=" + part(h.Done());
    }
    out += ";rs=" + part(Short(ci.pRasterizationState));
    if (auto* ms = ci.pMultisampleState) {
        Hasher h;
        auto copy = *ms;
        copy.pSampleMask = nullptr;
        h.AddBytes(reinterpret_cast<const u8*>(&copy) + 16, sizeof(copy) - 16);
        if (ms->pSampleMask) h.Add(ms->pSampleMask[0]);
        AddChainTypes(h, ms->pNext);
        out += ";ms=" + part(h.Done());
    }
    out += ";ds=" + part(Short(ci.pDepthStencilState));
    if (auto* cb = ci.pColorBlendState) {
        Hasher h;
        h.Add(cb->flags);
        h.Add(cb->logicOpEnable);
        h.Add(cb->logicOp);
        h.Add(cb->blendConstants);
        for (u32 i = 0; cb->pAttachments && i < cb->attachmentCount; ++i) h.Add(cb->pAttachments[i]);
        AddChainTypes(h, cb->pNext);
        out += ";cb=" + part(h.Done());
    }
    if (auto* dyn = ci.pDynamicState) {
        Hasher h;
        h.AddBytes(dyn->pDynamicStates, dyn->dynamicStateCount * sizeof(VkDynamicState));
        out += ";dyn=" + part(h.Done());
    }
    if (auto* rend = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(
            FindInChain(ci.pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO))) {
        Hasher h;
        h.Add(rend->viewMask);
        h.AddBytes(rend->pColorAttachmentFormats, rend->colorAttachmentCount * sizeof(VkFormat));
        h.Add(rend->depthAttachmentFormat);
        h.Add(rend->stencilAttachmentFormat);
        out += ";rend=" + part(h.Done());
    }
    {
        Hasher h;
        AddChainTypes(h, ci.pNext);
        for (u32 i = 0; i < ci.stageCount; ++i) {
            h.Add(ci.pStages[i].flags);
            h.Add(ci.pStages[i].stage);
            h.AddBytes(ci.pStages[i].pName, ci.pStages[i].pName ? std::strlen(ci.pStages[i].pName) : 0);
            AddChainTypes(h, ci.pStages[i].pNext);
        }
        out += ";chain=" + part(h.Done());
    }
    out += ";layout=" + part(ObjectHash(reinterpret_cast<u64>(ci.layout)));
    return out;
}

// One row per pipeline; feedback may be invalid when the driver does not fill it.
void LogPipeline(VkDevice device, const char* kind, VkPipeline pipeline, double ms,
                 const VkPipelineCreationFeedback& whole,
                 const VkPipelineCreationFeedback* stages, const VkPipelineShaderStageCreateInfo* infos,
                 u32 stage_count, VkResult result, bool captured, const std::string& spirv,
                 const std::string& state) {
    const bool valid = whole.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT;
    const bool hit = whole.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
    std::string stage_text;
    for (u32 i = 0; stages && i < stage_count; ++i) {
        if (!(stages[i].flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)) continue;
        stage_text += fmt::format("{}{}={:.2f}{}", stage_text.empty() ? "" : ";",
                                  StageName(infos[i].stage), stages[i].duration / 1e6,
                                  (stages[i].flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) ? "h" : "");
    }
    const double driver_ms = valid ? whole.duration / 1e6 : -1.0;
    WriteRow(fmt::format("{:.3f},{},{},{:#x},{:.3f},{:.3f},{},{},{},{},{},{}", NowMs(), ThreadId(), kind,
                         reinterpret_cast<u64>(pipeline), ms, driver_ms,
                         valid ? (hit ? "hit" : "miss") : "unknown", Quote(stage_text),
                         static_cast<int>(result), Quote(Statistics(device, pipeline, captured && result == VK_SUCCESS)), Quote(spirv), Quote(state)));
    if (pipeline && insert_label) {
        std::scoped_lock lock{pending_mutex};
        pending[pipeline] = fmt::format("RyGT compile {} {:.1f} ms {}", kind, ms,
                                        valid ? (hit ? "cache hit" : "cache miss") : "");
        pending_count.store(pending.size(), std::memory_order_relaxed);
    }
}

template <typename CreateInfo>
const char* KindOf(const CreateInfo& info, const char* whole) {
    const VkPipelineCreateFlags2 flags = [&]() -> VkPipelineCreateFlags2 {
        if (auto* f2 = reinterpret_cast<const VkPipelineCreateFlags2CreateInfo*>(FindInChain(
                info.pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO))) {
            return f2->flags;
        }
        return info.flags;
    }();
    if (flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) return "library";
    if (FindInChain(info.pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR)) {
        return (flags & VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT) ? "link-optimized"
                                                                          : "link";
    }
    if (FindInChain(info.pNext, VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR)) return "from-binary";
    return whole;
}

template <typename CreateInfo, typename Real>
VkResult CreateLogged(VkDevice device, VkPipelineCache cache, u32 count, const CreateInfo* infos,
                      const VkAllocationCallbacks* alloc, VkPipeline* pipelines, Real real,
                      const char* whole_kind) {
    std::vector<CreateInfo> copies(infos, infos + count);
    std::vector<VkPipelineCreationFeedback> whole(count);
    std::vector<std::vector<VkPipelineCreationFeedback>> stages(count);
    std::vector<VkPipelineCreationFeedbackCreateInfo> feedback(count);
    std::vector<const VkPipelineCreationFeedbackCreateInfo*> theirs(count);
    std::vector<const char*> kinds(count);
    std::vector<char> captured(count, 0);
    for (u32 i = 0; i < count; ++i) {
        auto& ci = copies[i];
        kinds[i] = KindOf(ci, whole_kind);
        theirs[i] = reinterpret_cast<const VkPipelineCreationFeedbackCreateInfo*>(
            FindInChain(ci.pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO));
        if (!theirs[i]) {
            u32 stage_count;
            if constexpr (std::is_same_v<CreateInfo, VkGraphicsPipelineCreateInfo>) {
                stage_count = ci.stageCount;
            } else {
                stage_count = 1;
            }
            stages[i].resize(stage_count);
            feedback[i] = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, ci.pNext,
                           &whole[i], stage_count, stages[i].data()};
            ci.pNext = &feedback[i];
        }
        // Statistics must be asked for at creation; libraries, binaries and flags2 chains are
        // left as they are so the build matches a normal run.
        if (capture_statistics && kinds[i] == whole_kind &&
            !FindInChain(infos[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO)) {
            ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
            captured[i] = 1;
        }
    }
    const auto begin = std::chrono::steady_clock::now();
    const VkResult result = real(device, cache, count, copies.data(), alloc, pipelines);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() /
        std::max<u32>(count, 1);
    // Level 3: build each whole pipeline a second time at once with the same cache. If even
    // this repeat misses, the driver does not keep whole pipelines in the application cache.
    if (Level() == 3 && result == VK_SUCCESS && destroy_pipeline) {
        for (u32 i = 0; i < count; ++i) {
            if (kinds[i] != whole_kind || theirs[i]) continue;
            VkPipelineCreationFeedback repeat_whole{};
            std::vector<VkPipelineCreationFeedback> repeat_stages(stages[i].size());
            VkPipelineCreationFeedbackCreateInfo repeat_fb = feedback[i];
            repeat_fb.pPipelineCreationFeedback = &repeat_whole;
            repeat_fb.pPipelineStageCreationFeedbacks = repeat_stages.data();
            CreateInfo again = copies[i];
            again.pNext = &repeat_fb;
            VkPipeline repeat = VK_NULL_HANDLE;
            const auto t0 = std::chrono::steady_clock::now();
            const VkResult r = real(device, cache, 1, &again, alloc, &repeat);
            const double repeat_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const bool valid = repeat_whole.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT;
            const bool hit = repeat_whole.flags &
                             VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
            WriteRow(fmt::format("{:.3f},{},{}-repeat,{:#x},{:.3f},{:.3f},{},\"\",{},\"\",\"\",\"\"",
                                 NowMs(), ThreadId(), whole_kind, reinterpret_cast<u64>(pipelines[i]),
                                 repeat_ms, valid ? repeat_whole.duration / 1e6 : -1.0,
                                 valid ? (hit ? "hit" : "miss") : "unknown", static_cast<int>(r)));
            if (repeat) destroy_pipeline(device, repeat, alloc);
        }
    }
    for (u32 i = 0; i < count; ++i) {
        const auto* fb = theirs[i] ? theirs[i] : &feedback[i];
        const VkPipelineShaderStageCreateInfo* stage_infos;
        if constexpr (std::is_same_v<CreateInfo, VkGraphicsPipelineCreateInfo>) {
            stage_infos = copies[i].pStages;
        } else {
            stage_infos = &copies[i].stage;
        }
        LogPipeline(device, kinds[i], pipelines[i], ms, *fb->pPipelineCreationFeedback,
                    fb->pPipelineStageCreationFeedbacks, stage_infos,
                    fb->pipelineStageCreationFeedbackCount, result, captured[i] != 0,
                    StageHashes(stage_infos, [&]() -> u32 {
                        if constexpr (std::is_same_v<CreateInfo, VkGraphicsPipelineCreateInfo>) {
                            return copies[i].stageCount;
                        } else {
                            return 1;
                        }
                    }()),
                    [&]() -> std::string {
                        if constexpr (std::is_same_v<CreateInfo, VkGraphicsPipelineCreateInfo>) {
                            return StateHashes(copies[i]);
                        } else {
                            return fmt::format("layout={:08x}", static_cast<u32>(ObjectHash(
                                reinterpret_cast<u64>(copies[i].layout))));
                        }
                    }());
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphics(VkDevice device, VkPipelineCache cache, u32 count,
                                              const VkGraphicsPipelineCreateInfo* infos,
                                              const VkAllocationCallbacks* alloc,
                                              VkPipeline* pipelines) {
    return CreateLogged(device, cache, count, infos, alloc, pipelines, real_create_graphics,
                        "graphics");
}

VKAPI_ATTR VkResult VKAPI_CALL CreateCompute(VkDevice device, VkPipelineCache cache, u32 count,
                                             const VkComputePipelineCreateInfo* infos,
                                             const VkAllocationCallbacks* alloc,
                                             VkPipeline* pipelines) {
    return CreateLogged(device, cache, count, infos, alloc, pipelines, real_create_compute,
                        "compute");
}

VKAPI_ATTR VkResult VKAPI_CALL CreateShaders(VkDevice device, u32 count,
                                             const VkShaderCreateInfoEXT* infos,
                                             const VkAllocationCallbacks* alloc,
                                             VkShaderEXT* shaders) {
    const auto begin = std::chrono::steady_clock::now();
    const VkResult result = real_create_shaders(device, count, infos, alloc, shaders);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    std::string stages;
    for (u32 i = 0; i < count; ++i) stages += fmt::format("{}{}", i ? ";" : "", StageName(infos[i].stage));
    WriteRow(fmt::format("{:.3f},{},shader-object,{:#x},{:.3f},-1,unknown,{},{},\"\",\"\",\"\"", NowMs(),
                         ThreadId(), count ? reinterpret_cast<u64>(shaders[0]) : 0, ms,
                         Quote(stages), static_cast<int>(result)));
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL SetName(VkDevice device, const VkDebugUtilsObjectNameInfoEXT* info) {
    if (info && info->pObjectName &&
        (info->objectType == VK_OBJECT_TYPE_PIPELINE || info->objectType == VK_OBJECT_TYPE_SHADER_EXT)) {
        WriteRow(fmt::format("{:.3f},{},name,{:#x},0,0,,{},0,\"\",\"\",\"\"", NowMs(), ThreadId(),
                             info->objectHandle, Quote(info->pObjectName)));
        if (info->objectType == VK_OBJECT_TYPE_PIPELINE && pending_count.load(std::memory_order_relaxed)) {
            std::scoped_lock lock{pending_mutex};
            if (auto it = pending.find(reinterpret_cast<VkPipeline>(info->objectHandle)); it != pending.end()) {
                it->second += fmt::format(" | {}", info->pObjectName);
            }
        }
    }
    return real_set_name ? real_set_name(device, info) : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Bind(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {
    if (pending_count.load(std::memory_order_relaxed)) {
        std::string label;
        {
            std::scoped_lock lock{pending_mutex};
            if (auto it = pending.find(pipeline); it != pending.end()) {
                label = std::move(it->second);
                pending.erase(it);
                pending_count.store(pending.size(), std::memory_order_relaxed);
            }
        }
        if (!label.empty()) {
            const VkDebugUtilsLabelEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, nullptr,
                                            label.c_str(), {1.0f, 0.3f, 0.1f, 1.0f}};
            insert_label(cmd, &info);
        }
    }
    real_bind(cmd, point, pipeline);
}

} // namespace

int Level() {
    static const int level = [] {
        const char* value = std::getenv("RYGT_PIPELINE_COMPILE_LOG");
        return value && *value ? std::atoi(value) : 0;
    }();
    return level;
}

bool WantsExecutableProperties() {
    return Level() == 2;
}

void Install(vk::Device device, bool executable_properties) {
    if (Level() <= 0) return;
    auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "pipeline_compiles.csv";
    {
        std::scoped_lock lock{file_mutex};
#ifdef _WIN32
        file = _wfopen(path.c_str(), L"w");
#else
        file = std::fopen(path.c_str(), "w");
#endif
        if (!file) {
            LOG_ERROR(Render_Vulkan, "DIAG-SHC-001: cannot write {}", path.string());
            return;
        }
        std::fputs("ms,thread,kind,handle,call_ms,driver_ms,cache,stages,result,statistics,spirv,state\n", file);
        std::fflush(file);
    }
    capture_statistics = executable_properties;
    destroy_pipeline = d.vkDestroyPipeline;
    get_executables = d.vkGetPipelineExecutablePropertiesKHR;
    get_statistics = d.vkGetPipelineExecutableStatisticsKHR;
    insert_label = d.vkCmdInsertDebugUtilsLabelEXT;
    if (d.vkCreateGraphicsPipelines) {
        real_create_graphics = d.vkCreateGraphicsPipelines;
        d.vkCreateGraphicsPipelines = CreateGraphics;
    }
    if (d.vkCreateComputePipelines) {
        real_create_compute = d.vkCreateComputePipelines;
        d.vkCreateComputePipelines = CreateCompute;
    }
    if (d.vkCreateSampler && d.vkCreateDescriptorSetLayout && d.vkCreatePipelineLayout) {
        real_create_sampler = d.vkCreateSampler;
        d.vkCreateSampler = CreateSampler;
        real_create_set_layout = d.vkCreateDescriptorSetLayout;
        d.vkCreateDescriptorSetLayout = CreateSetLayout;
        real_create_layout = d.vkCreatePipelineLayout;
        d.vkCreatePipelineLayout = CreateLayout;
    }
    if (d.vkCreateShaderModule) {
        real_create_module = d.vkCreateShaderModule;
        d.vkCreateShaderModule = CreateModule;
    }
    if (d.vkCreateShadersEXT) {
        real_create_shaders = d.vkCreateShadersEXT;
        d.vkCreateShadersEXT = CreateShaders;
    }
    real_set_name = d.vkSetDebugUtilsObjectNameEXT;
    d.vkSetDebugUtilsObjectNameEXT = SetName;
    if (insert_label && d.vkCmdBindPipeline) {
        real_bind = d.vkCmdBindPipeline;
        d.vkCmdBindPipeline = Bind;
    }
    LOG_WARNING(Render_Vulkan, "DIAG-SHC-001: pipeline compile log on (level {}, driver statistics {}), {}",
                Level(), capture_statistics ? "on" : "off", path.string());
}

} // namespace Vulkan::PipelineCompileLog
