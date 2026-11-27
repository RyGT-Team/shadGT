// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// FIX-SGT-192: one small driver pipeline cache per pipeline, on disk, for drivers without
// VK_KHR_pipeline_binary (Intel Arc on Windows has none: RyGT #128) or where it is off.
//
// The single driver cache file was loaded whole and the driver keeps all of it in memory, so GT7
// moved a file over 1 GB aside at start (FIX-SGT-010) and a heavy player compiled everything
// again. Here every pipeline gets a VkPipelineCache of its own, made from its own file just before
// the pipeline is created and destroyed just after: only the pipelines a session builds are read,
// and none stays in memory. The file name is a key that stays the same from run to run (the
// stages' SPIR-V hashes and the state, not handles); the driver checks the contents itself, so a
// key that names the wrong pipeline is only a compile, never a wrong pipeline. A miss (the
// creation feedback says the cache was not used) writes the file again.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <list>
#include <span>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <xxhash.h>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::PipelineCacheFiles {

namespace detail {

inline VkResult Make(vk::Device device, VkPipelineCache cache,
                     const VkGraphicsPipelineCreateInfo& info, VkPipeline* pipeline) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateGraphicsPipelines(device, cache, 1, &info,
                                                                   nullptr, pipeline);
}

inline VkResult Make(vk::Device device, VkPipelineCache cache,
                     const VkComputePipelineCreateInfo& info, VkPipeline* pipeline) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateComputePipelines(device, cache, 1, &info,
                                                                  nullptr, pipeline);
}

struct Stats {
    std::atomic<u64> hits{};        // made from its file, the driver says from the cache
    std::atomic<u64> written{};     // compiled, its file written
    std::atomic<u64> rewritten{};   // of those, a file that was there but missed
    std::atomic<u64> unchanged{};   // a miss or no feedback, but the file already had it all
    std::atomic<u64> no_key{};      // a module or library the key cannot name: the shared cache
    std::atomic<u64> refused{};     // a file the driver refused: started empty
    std::atomic<u64> write_failed{};
    std::atomic<u64> bytes_read{};
};

inline Stats& GetStats() {
    static Stats stats;
    return stats;
}

// The keys of pipelines (libraries) made here, so a pipeline linked from them has a key too.
struct Libraries {
    std::mutex mutex;
    std::unordered_map<VkPipeline, u64> keys;
};

inline Libraries& GetLibraries() {
    static Libraries libraries;
    return libraries;
}

inline u64 LibraryKey(VkPipeline pipeline) {
    auto& libraries = GetLibraries();
    std::scoped_lock lock{libraries.mutex};
    const auto it = libraries.keys.find(pipeline);
    return it != libraries.keys.end() ? it->second : 0;
}

inline void RememberLibrary(VkPipeline pipeline, u64 key) {
    auto& libraries = GetLibraries();
    std::scoped_lock lock{libraries.mutex};
    libraries.keys.insert_or_assign(pipeline, key);
}

class Hasher {
public:
    void Bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const u8*>(data);
        if (size) bytes.insert(bytes.end(), p, p + size);
    }
    template <class T>
    void Add(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        Bytes(&value, sizeof(T));
    }
    // A structure with no pointers after its sType and pNext: the rest of it, as bytes.
    template <class T>
    void Tail(const T* value) {
        static_assert(offsetof(T, pNext) + sizeof(void*) <= sizeof(T));
        constexpr std::size_t start = offsetof(T, pNext) + sizeof(void*);
        Add(static_cast<u32>(value->sType));
        Bytes(reinterpret_cast<const char*>(value) + start, sizeof(T) - start);
    }
    u64 Digest() {
        return XXH3_64bits(bytes.data(), bytes.size()) | 1; // never 0
    }
    bool ok = true;

private:
    std::vector<u8> bytes;
};

// A chain of the structures the renderer puts there. Known ones by value; any other by its type,
// which can only make two pipelines share a key (a compile, never a wrong pipeline).
inline void Chain(Hasher& h, const void* next) {
    for (auto* base = static_cast<const VkBaseInStructure*>(next); base; base = base->pNext) {
        switch (base->sType) {
        case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT:
            h.Tail(reinterpret_cast<const VkGraphicsPipelineLibraryCreateInfoEXT*>(base));
            break;
        case VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR:
            h.Tail(reinterpret_cast<const VkPipelineCreateFlags2CreateInfoKHR*>(base));
            break;
        case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO: {
            const auto* r = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(base);
            h.Add(r->sType);
            h.Add(r->viewMask);
            h.Add(r->colorAttachmentCount);
            h.Bytes(r->pColorAttachmentFormats, r->colorAttachmentCount * sizeof(VkFormat));
            h.Add(r->depthAttachmentFormat);
            h.Add(r->stencilAttachmentFormat);
            break;
        }
        case VK_STRUCTURE_TYPE_ATTACHMENT_SAMPLE_COUNT_INFO_AMD: {
            const auto* s = reinterpret_cast<const VkAttachmentSampleCountInfoAMD*>(base);
            h.Add(s->sType);
            h.Add(s->colorAttachmentCount);
            h.Bytes(s->pColorAttachmentSamples,
                    s->colorAttachmentCount * sizeof(VkSampleCountFlagBits));
            h.Add(s->depthStencilAttachmentSamples);
            break;
        }
        case VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR: {
            const auto* l = reinterpret_cast<const VkPipelineLibraryCreateInfoKHR*>(base);
            h.Add(l->sType);
            h.Add(l->libraryCount);
            for (u32 i = 0; i < l->libraryCount; ++i) {
                const u64 key = LibraryKey(l->pLibraries[i]);
                if (key == 0) h.ok = false; // a library made without a key
                h.Add(key);
            }
            break;
        }
        case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO:
            h.Tail(reinterpret_cast<const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo*>(base));
            break;
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT:
            h.Tail(reinterpret_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT*>(base));
            break;
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT:
            h.Tail(reinterpret_cast<const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT*>(base));
            break;
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT:
            h.Tail(reinterpret_cast<const VkPipelineRasterizationLineStateCreateInfoEXT*>(base));
            break;
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT:
            h.Tail(reinterpret_cast<const VkPipelineRasterizationConservativeStateCreateInfoEXT*>(base));
            break;
        case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT:
            h.Tail(reinterpret_cast<const VkPipelineViewportDepthClipControlCreateInfoEXT*>(base));
            break;
        case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
            h.Tail(reinterpret_cast<const VkPipelineTessellationDomainOriginStateCreateInfo*>(base));
            break;
        default:
            h.Add(base->sType);
            break;
        }
    }
}

inline void Stage(Hasher& h, const VkPipelineShaderStageCreateInfo& stage) {
    h.Add(stage.flags);
    h.Add(stage.stage);
    const u64 code = ModuleHash(vk::ShaderModule{stage.module});
    if (code == 0) h.ok = false; // no module (inline code) or one made without its hash
    h.Add(code);
    const std::string_view name = stage.pName ? stage.pName : "";
    h.Bytes(name.data(), name.size());
    if (const auto* spec = stage.pSpecializationInfo) {
        h.Add(spec->mapEntryCount);
        h.Bytes(spec->pMapEntries, spec->mapEntryCount * sizeof(VkSpecializationMapEntry));
        h.Bytes(spec->pData, spec->dataSize);
    }
    Chain(h, stage.pNext);
}

// 0: no key (the pipeline goes to the shared driver cache, as before).
inline u64 KeyOf(const VkGraphicsPipelineCreateInfo& info) {
    Hasher h;
    h.Add(u32{1}); // graphics; bump to give every key a new name
    h.Add(info.flags);
    Chain(h, info.pNext);
    h.Add(info.stageCount);
    for (u32 i = 0; i < info.stageCount; ++i) Stage(h, info.pStages[i]);
    if (const auto* v = info.pVertexInputState) {
        h.Add(v->flags);
        h.Add(v->vertexBindingDescriptionCount);
        h.Bytes(v->pVertexBindingDescriptions,
                v->vertexBindingDescriptionCount * sizeof(VkVertexInputBindingDescription));
        h.Add(v->vertexAttributeDescriptionCount);
        h.Bytes(v->pVertexAttributeDescriptions,
                v->vertexAttributeDescriptionCount * sizeof(VkVertexInputAttributeDescription));
        Chain(h, v->pNext);
    }
    if (const auto* a = info.pInputAssemblyState) h.Tail(a), Chain(h, a->pNext);
    if (const auto* t = info.pTessellationState) h.Tail(t), Chain(h, t->pNext);
    if (const auto* v = info.pViewportState) {
        h.Add(v->flags);
        h.Add(v->viewportCount);
        h.Add(v->scissorCount);
        if (v->pViewports) h.Bytes(v->pViewports, v->viewportCount * sizeof(VkViewport));
        if (v->pScissors) h.Bytes(v->pScissors, v->scissorCount * sizeof(VkRect2D));
        Chain(h, v->pNext);
    }
    if (const auto* r = info.pRasterizationState) h.Tail(r), Chain(h, r->pNext);
    if (const auto* m = info.pMultisampleState) {
        h.Add(m->flags);
        h.Add(m->rasterizationSamples);
        h.Add(m->sampleShadingEnable);
        h.Add(m->minSampleShading);
        h.Add(m->alphaToCoverageEnable);
        h.Add(m->alphaToOneEnable);
        if (m->pSampleMask) h.Bytes(m->pSampleMask, ((m->rasterizationSamples + 31) / 32) * 4);
        Chain(h, m->pNext);
    }
    if (const auto* d = info.pDepthStencilState) h.Tail(d), Chain(h, d->pNext);
    if (const auto* c = info.pColorBlendState) {
        h.Add(c->flags);
        h.Add(c->logicOpEnable);
        h.Add(c->logicOp);
        h.Add(c->attachmentCount);
        if (c->pAttachments) {
            h.Bytes(c->pAttachments,
                    c->attachmentCount * sizeof(VkPipelineColorBlendAttachmentState));
        }
        h.Add(c->blendConstants);
        Chain(h, c->pNext);
    }
    if (const auto* d = info.pDynamicState) {
        h.Add(d->dynamicStateCount);
        h.Bytes(d->pDynamicStates, d->dynamicStateCount * sizeof(VkDynamicState));
    }
    h.Add(info.renderPass != VK_NULL_HANDLE);
    h.Add(info.subpass);
    // The layout is left out: with the same SPIR-V it is the same but for a handle.
    return h.ok ? h.Digest() : 0;
}

inline u64 KeyOf(const VkComputePipelineCreateInfo& info) {
    Hasher h;
    h.Add(u32{2}); // compute
    h.Add(info.flags);
    Chain(h, info.pNext);
    Stage(h, info.stage);
    return h.ok ? h.Digest() : 0;
}

inline std::string DriverIdentity(const Instance& instance) {
    const auto props = instance.GetPhysicalDevice().getProperties();
    std::string uuid;
    for (const auto byte : props.pipelineCacheUUID) uuid += fmt::format("{:02x}", byte);
    return fmt::format("{:04x}-{:04x}-{:08x}-{}", props.vendorID, props.deviceID,
                       props.driverVersion, uuid);
}

// The folder for this game and driver (a new driver starts a new one); empty when unusable.
inline const std::filesystem::path& Folder(const Instance& instance) {
    static std::once_flag once;
    static std::filesystem::path folder;
    std::call_once(once, [&instance] {
        const auto& serial = Common::Singleton<Common::ElfInfo>::Instance()->GameSerial();
        auto path = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
                    (std::string{serial} + "-pipelines-" + DriverIdentity(instance));
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error) {
            LOG_WARNING(Render_Vulkan, "FIX-SGT-192: cannot create {}: {}", path.string(),
                        error.message());
            return;
        }
        folder = std::move(path);
        LOG_WARNING(Render_Vulkan, "FIX-SGT-192: one driver cache file per pipeline in {}",
                    folder.string());
    });
    return folder;
}

inline bool ReadWhole(const std::filesystem::path& path, std::vector<u8>& data) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) return false;
    const auto size = static_cast<std::streamoff>(file.tellg());
    if (size <= 0 || size > (std::streamoff{256} << 20)) return false;
    data.resize(static_cast<std::size_t>(size));
    file.seekg(0);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(data.data()), size));
}

// A temporary of this thread's own, then a rename: two builds of one pipeline never share it.
inline bool WriteWhole(const std::filesystem::path& path, const u8* data, std::size_t size) {
    auto temp = path;
    temp += fmt::format(".{}.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream file{temp, std::ios::binary | std::ios::trunc};
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!file) return false;
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    if (error) std::filesystem::remove(temp, error);
    return !error;
}

// Chunks: NVIDIA's driver puts the same 2 MB block in every pipeline's cache data, at no fixed
// place (beside 5-150 KB of the pipeline's own); whole files took 7.2 GB in four minutes of GT7.
// The data is cut where its content says (a gear hash, so the shared block is cut the same way
// wherever it sits), each piece is kept once under its hash, and a pipeline's file lists them.
namespace chunk {

inline constexpr std::size_t Min = 8 << 10;
inline constexpr std::size_t Max = 128 << 10;
inline constexpr u64 Mask = (u64{1} << 15) - 1; // about 32 KB past the minimum on average
inline constexpr u32 Magic = 0x31434350;          // "PCC1"

inline const std::array<u64, 256>& Gear() {
    static const std::array<u64, 256> table = [] {
        std::array<u64, 256> t{};
        u64 x = 0x9E3779B97F4A7C15ull; // splitmix64: the same table on every run
        for (auto& v : t) {
            x += 0x9E3779B97F4A7C15ull;
            u64 z = x;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            v = z ^ (z >> 31);
        }
        return t;
    }();
    return table;
}

// The ends of the chunks of `data`.
inline std::vector<std::size_t> Cuts(std::span<const u8> data) {
    const auto& gear = Gear();
    std::vector<std::size_t> cuts;
    std::size_t start = 0;
    while (start < data.size()) {
        const std::size_t end = std::min(data.size(), start + Max);
        std::size_t at = std::min(end, start + Min);
        u64 hash = 0;
        for (; at < end; ++at) {
            hash = (hash << 1) + gear[data[at]];
            if ((hash & Mask) == 0) {
                ++at;
                break;
            }
        }
        cuts.push_back(at);
        start = at;
    }
    return cuts;
}

struct Ref {
    u64 hash;
    u32 size;
};

// Recently read chunks, so the shared block is not read from disk for every pipeline.
struct Memo {
    std::mutex mutex;
    std::list<std::pair<u64, std::vector<u8>>> order; // most recent first
    std::unordered_map<u64, decltype(order)::iterator> index;
    std::size_t bytes = 0;
    static constexpr std::size_t Limit = 16 << 20;

    bool Get(u64 hash, std::vector<u8>& out) {
        std::scoped_lock lock{mutex};
        const auto it = index.find(hash);
        if (it == index.end()) return false;
        order.splice(order.begin(), order, it->second);
        out = it->second->second;
        return true;
    }
    void Put(u64 hash, std::span<const u8> data) {
        std::scoped_lock lock{mutex};
        if (index.contains(hash)) return;
        order.emplace_front(hash, std::vector<u8>(data.begin(), data.end()));
        index.emplace(hash, order.begin());
        bytes += data.size();
        while (bytes > Limit && !order.empty()) {
            bytes -= order.back().second.size();
            index.erase(order.back().first);
            order.pop_back();
        }
    }
};

inline Memo& GetMemo() {
    static Memo memo;
    return memo;
}

inline std::filesystem::path PathOf(const std::filesystem::path& folder, const Ref& ref) {
    return folder / "chunks" / fmt::format("{:016x}-{:x}", ref.hash, ref.size);
}

} // namespace chunk

// A pipeline's file: u32 "PCC1", u64 size, u64 XXH3 of the data, u32 chunk count, then per
// chunk u64 hash and u32 size. The data is checked whole after it is put together.
inline bool ReadFile(const std::filesystem::path& path, std::vector<u8>& data) {
    std::vector<u8> index;
    if (!ReadWhole(path, index) || index.size() < 24) return false;
    u32 magic, count;
    u64 size, hash;
    std::memcpy(&magic, index.data(), 4);
    std::memcpy(&size, index.data() + 4, 8);
    std::memcpy(&hash, index.data() + 12, 8);
    std::memcpy(&count, index.data() + 20, 4);
    if (magic != chunk::Magic || size > (u64{256} << 20) ||
        index.size() != 24 + std::size_t{count} * 12) {
        return false;
    }
    data.clear();
    data.reserve(size);
    std::vector<u8> piece;
    for (u32 i = 0; i < count; ++i) {
        chunk::Ref ref;
        std::memcpy(&ref.hash, index.data() + 24 + i * 12, 8);
        std::memcpy(&ref.size, index.data() + 32 + i * 12, 4);
        if (!chunk::GetMemo().Get(ref.hash, piece)) {
            if (!ReadWhole(chunk::PathOf(path.parent_path(), ref), piece)) return false;
            chunk::GetMemo().Put(ref.hash, piece);
        }
        if (piece.size() != ref.size) return false;
        data.insert(data.end(), piece.begin(), piece.end());
    }
    return data.size() == size && XXH3_64bits(data.data(), data.size()) == hash;
}

inline bool WriteFile(const std::filesystem::path& path, const std::vector<u8>& data) {
    const auto folder = path.parent_path();
    std::error_code error;
    std::filesystem::create_directories(folder / "chunks", error);
    std::vector<chunk::Ref> refs;
    std::size_t start = 0;
    for (const std::size_t end : chunk::Cuts(data)) {
        const chunk::Ref ref{XXH3_64bits(data.data() + start, end - start),
                             static_cast<u32>(end - start)};
        const auto chunk_path = chunk::PathOf(folder, ref);
        // A chunk already kept is not written again.
        if (!std::filesystem::exists(chunk_path, error) &&
            !WriteWhole(chunk_path, data.data() + start, end - start)) {
            return false;
        }
        refs.push_back(ref);
        start = end;
    }
    std::vector<u8> index(24 + refs.size() * 12);
    const u32 magic = chunk::Magic;
    const u64 size = data.size();
    const u64 hash = XXH3_64bits(data.data(), data.size());
    const u32 count = static_cast<u32>(refs.size());
    std::memcpy(index.data(), &magic, 4);
    std::memcpy(index.data() + 4, &size, 8);
    std::memcpy(index.data() + 12, &hash, 8);
    std::memcpy(index.data() + 20, &count, 4);
    for (u32 i = 0; i < count; ++i) {
        std::memcpy(index.data() + 24 + i * 12, &refs[i].hash, 8);
        std::memcpy(index.data() + 32 + i * 12, &refs[i].size, 4);
    }
    return WriteWhole(path, index.data(), index.size());
}

inline void Report() {
    auto& s = GetStats();
    const u64 total = s.hits + s.written + s.unchanged;
    if (total != 0 && total % 500 == 0) {
        LOG_WARNING(Render_Vulkan,
                    "FIX-SGT-192: {} pipelines from their cache files, {} compiled and written ({} of them over a file that missed), "
                    "{} unchanged, {} without a key (shared cache), {} files refused, {} writes "
                    "failed, {} MB read",
                    s.hits.load(), s.written.load(), s.rewritten.load(), s.unchanged.load(), s.no_key.load(),
                    s.refused.load(), s.write_failed.load(), s.bytes_read.load() >> 20);
    }
}

} // namespace detail

/// Whether pipelines get a cache file each (Instance decides: SHADGT_PIPELINE_CACHE_FILES).
inline bool On(const Instance& instance) {
    return instance.IsPipelineCacheFiles();
}

/// Whether a create info has a key and its file exists (a warm-up has nothing to do then).
template <class RawInfo>
bool Stored(const Instance& instance, const RawInfo& raw) {
    const auto& folder = detail::Folder(instance);
    const u64 key = detail::KeyOf(raw);
    std::error_code error;
    return key != 0 && !folder.empty() &&
           std::filesystem::exists(folder / fmt::format("{:016x}.vkpc", key), error);
}

/// Creates a pipeline with a cache of its own; `shared` (the shared driver cache) when the create
/// info has no key. A library's key is remembered for the pipelines linked from it.
template <class RawInfo>
VkResult Create(const Instance& instance, vk::Device device, vk::PipelineCache shared,
                const RawInfo& raw, VkPipeline* pipeline) {
    using namespace detail;
    const auto& folder = Folder(instance);
    const u64 key = KeyOf(raw);
    if (key == 0 || folder.empty()) {
        if (key == 0) ++GetStats().no_key;
        return Make(device, shared, raw, pipeline);
    }
    const auto path = folder / fmt::format("{:016x}.vkpc", key);
    std::vector<u8> data;
    const bool had_file = ReadFile(path, data);
    if (had_file) GetStats().bytes_read += data.size();

    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0,
                                         data.size(), data.data()};
    VkPipelineCache cache = VK_NULL_HANDLE;
    if (d.vkCreatePipelineCache(device, &cache_info, nullptr, &cache) != VK_SUCCESS) {
        ++GetStats().refused;
        cache_info.initialDataSize = 0;
        cache_info.pInitialData = nullptr;
        if (d.vkCreatePipelineCache(device, &cache_info, nullptr, &cache) != VK_SUCCESS) {
            return Make(device, shared, raw, pipeline);
        }
    }

    // The creation feedback says whether the cache was used.
    VkPipelineCreationFeedback feedback{};
    const VkPipelineCreationFeedbackCreateInfo feedback_info{
        VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, raw.pNext, &feedback, 0, nullptr};
    RawInfo with_feedback = raw;
    with_feedback.pNext = &feedback_info;
    const VkResult result =
        Make(device, cache, with_feedback, pipeline);
    if (result == VK_SUCCESS) {
        const bool valid = feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT;
        const bool hit = valid && (feedback.flags &
                                   VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT);
        if (hit) {
            ++GetStats().hits;
        } else {
            // A miss, or a driver that doesn't say: write the file unless it has all of it.
            std::size_t size = 0;
            if (d.vkGetPipelineCacheData(device, cache, &size, nullptr) == VK_SUCCESS &&
                size != 0 && !(had_file && size == data.size())) {
                std::vector<u8> out(size);
                if (d.vkGetPipelineCacheData(device, cache, &size, out.data()) == VK_SUCCESS) {
                    out.resize(size);
                    const bool wrote = WriteFile(path, out);
                    ++(wrote ? GetStats().written : GetStats().write_failed);
                    // A file that was there and still missed: the key named another pipeline,
                    // or the driver gave no feedback.
                    if (wrote && had_file) ++GetStats().rewritten;
                }
            } else {
                ++GetStats().unchanged;
            }
        }
        if constexpr (std::is_same_v<RawInfo, VkGraphicsPipelineCreateInfo>) {
            if (raw.flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) RememberLibrary(*pipeline, key);
        }
        Report();
    }
    d.vkDestroyPipelineCache(device, cache, nullptr);
    return result;
}

/// The totals so far, logged at shutdown.
inline void Summary() {
    auto& s = detail::GetStats();
    if (s.hits + s.written + s.unchanged + s.no_key == 0) return;
    LOG_WARNING(Render_Vulkan,
                "FIX-SGT-192: at exit {} pipelines from their cache files, {} compiled and "
                "written ({} of them over a file that missed), {} unchanged, {} without a key (shared cache), {} files refused, {} "
                "writes failed, {} MB read",
                s.hits.load(), s.written.load(), s.rewritten.load(), s.unchanged.load(), s.no_key.load(),
                s.refused.load(), s.write_failed.load(), s.bytes_read.load() >> 20);
}

} // namespace Vulkan::PipelineCacheFiles
