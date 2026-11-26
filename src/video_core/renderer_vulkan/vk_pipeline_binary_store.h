// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// BIN-001: an on-disk store of driver pipeline binaries (VK_KHR_pipeline_binary).
// AMD's Windows driver keeps nothing of its compiles in a VkPipelineCache, so GT Sport compiled
// every pipeline again on every launch. A pipeline built here is created with
// VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR the first time; its binaries are read out and written
// to <cache dir>/<serial>-binaries-<driver key>/<pipeline key>.bin. Later launches create the
// pipeline from those binaries, without compiling. The driver's global key names the folder, so
// a new driver starts a new one. Anything that fails falls back to an ordinary build.

#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache_files.h" // FIX-SGT-192

namespace Vulkan::PipelineBinaryStore {

namespace detail {

inline std::string Hex(const u8* bytes, u32 size) {
    static constexpr char Digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2);
    for (u32 i = 0; i < size; ++i) {
        text += Digits[bytes[i] >> 4];
        text += Digits[bytes[i] & 15];
    }
    return text;
}

struct Stats {
    std::atomic<u64> loaded{};
    std::atomic<u64> saved{};
    std::atomic<u64> failed{};
    // BIN-002: what each failure was.
    std::atomic<u64> unreadable{};      // a stored file could not be read
    std::atomic<u64> binaries_refused{}; // the driver refused the stored binaries
    std::atomic<u64> pipeline_refused{}; // the pipeline could not be made from them
    std::atomic<u64> nothing_captured{}; // the driver gave no binaries (no shaders, or none kept)
    std::atomic<u64> capture_failed{};   // reading the binaries out failed
    std::atomic<u64> write_failed{};
    std::atomic<u64> no_key{};
    std::atomic<u64> capture_refused{}; // FIX-SGT-192: the capturing create failed
    std::atomic<u64> libraries_loaded{};
    std::atomic<u64> libraries_saved{};
};

// BIN-002: the first failures of each kind, with the driver's result.
inline void Note(std::atomic<u64>& counter, const char* what, VkResult result) {
    if (++counter <= 5) {
        LOG_WARNING(Render_Vulkan, "BIN-002: {} ({})", what,
                    vk::to_string(vk::Result{result}));
    }
}

inline Stats& GetStats() {
    static Stats stats;
    return stats;
}

// The folder for this game and driver; empty when the global key is unavailable.
inline const std::filesystem::path& Folder(vk::Device device) {
    static std::once_flag once;
    static std::filesystem::path folder;
    std::call_once(once, [device] {
        const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
        VkPipelineBinaryKeyKHR global{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        if (!d.vkGetPipelineKeyKHR ||
            d.vkGetPipelineKeyKHR(device, nullptr, &global) != VK_SUCCESS) {
            LOG_WARNING(Render_Vulkan, "BIN-001: no global pipeline key; binaries are not stored");
            return;
        }
        const auto& serial = Common::Singleton<Common::ElfInfo>::Instance()->GameSerial();
        auto path = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
                    (std::string{serial} + "-binaries-" + Hex(global.key, global.keySize));
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error) {
            LOG_WARNING(Render_Vulkan, "BIN-001: cannot create {}: {}", path.string(),
                        error.message());
            return;
        }
        folder = std::move(path);
        LOG_WARNING(Render_Vulkan, "BIN-001: pipeline binaries in {}", folder.string());
    });
    return folder;
}

struct Entry {
    VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    std::vector<u8> data;
};

// FIX-SGT-192: a file of this thread's own beside `path`, then a rename (two builds of one
// pipeline shared ".tmp").
inline bool WriteWhole(const std::filesystem::path& path, const void* data, std::size_t size) {
    auto temp = path;
    temp += fmt::format(".{}.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream file{temp, std::ios::binary | std::ios::trunc};
        if (!file) return false;
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!file) return false;
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    if (error) std::filesystem::remove(temp, error);
    return !error;
}

// FIX-SGT-192: each binary is kept once, in a file named by its key. NVIDIA's driver gives every
// pipeline the same 2 MB binary (its key is the driver's cache UUID) beside the pipeline's own
// 5-100 KB one; stored inside each pipeline's file, GT7 wrote 6.7 GB in four minutes.
inline std::filesystem::path BlobPath(const std::filesystem::path& folder,
                                      const VkPipelineBinaryKeyKHR& key) {
    return folder / ("k-" + Hex(key.key, key.keySize) + ".blob");
}

inline constexpr u32 IndexMagic = 0x32494250; // "PBI2"

// Old file (BIN-001): u32 count, then per binary: u32 key size, key bytes, u64 data size, data.
// Index file (FIX-SGT-192): u32 "PBI2", u32 count, then per binary: u32 key size, key bytes, u64
// data size; the data in BlobPath.
inline bool Read(const std::filesystem::path& path, std::vector<Entry>& entries) {
    std::ifstream file{path, std::ios::binary};
    if (!file) return false;
    u32 count = 0;
    if (!file.read(reinterpret_cast<char*>(&count), sizeof(count))) return false;
    const bool index = count == IndexMagic;
    if (index && !file.read(reinterpret_cast<char*>(&count), sizeof(count))) return false;
    if (count == 0 || count > 64) return false;
    entries.resize(count);
    for (auto& entry : entries) {
        u32 key_size = 0;
        u64 data_size = 0;
        if (!file.read(reinterpret_cast<char*>(&key_size), sizeof(key_size)) ||
            key_size > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR ||
            !file.read(reinterpret_cast<char*>(entry.key.key), key_size) ||
            !file.read(reinterpret_cast<char*>(&data_size), sizeof(data_size)) ||
            data_size == 0 || data_size > (u64{256} << 20)) {
            return false;
        }
        entry.key.keySize = key_size;
        entry.data.resize(data_size);
        if (!index) {
            if (!file.read(reinterpret_cast<char*>(entry.data.data()), data_size)) return false;
            continue;
        }
        std::ifstream blob{BlobPath(path.parent_path(), entry.key),
                           std::ios::binary | std::ios::ate};
        if (!blob || static_cast<u64>(blob.tellg()) != data_size) return false;
        blob.seekg(0);
        if (!blob.read(reinterpret_cast<char*>(entry.data.data()), data_size)) return false;
    }
    return true;
}

inline bool Write(const std::filesystem::path& path, const std::vector<Entry>& entries) {
    std::vector<u8> index;
    const auto put = [&index](const void* data, std::size_t size) {
        const auto* p = static_cast<const u8*>(data);
        index.insert(index.end(), p, p + size);
    };
    const u32 magic = IndexMagic;
    const u32 count = static_cast<u32>(entries.size());
    put(&magic, sizeof(magic));
    put(&count, sizeof(count));
    for (const auto& entry : entries) {
        // A binary already kept is not written again (the same key is the same binary).
        const auto blob = BlobPath(path.parent_path(), entry.key);
        std::error_code error;
        const auto size = std::filesystem::file_size(blob, error);
        if ((error || size != entry.data.size()) &&
            !WriteWhole(blob, entry.data.data(), entry.data.size())) {
            return false;
        }
        const u32 key_size = entry.key.keySize;
        const u64 data_size = entry.data.size();
        put(&key_size, sizeof(key_size));
        put(entry.key.key, key_size);
        put(&data_size, sizeof(data_size));
    }
    return WriteWhole(path, index.data(), index.size());
}

inline void Destroy(vk::Device device, const std::vector<VkPipelineBinaryKHR>& binaries) {
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    for (const auto binary : binaries) {
        if (binary != VK_NULL_HANDLE) d.vkDestroyPipelineBinaryKHR(device, binary, nullptr);
    }
}

inline void Report() {
    auto& stats = GetStats();
    const u64 total = stats.loaded + stats.saved;
    if (total != 0 && total % 500 == 0) {
        LOG_WARNING(Render_Vulkan,
                    "BIN-001: {} pipelines from stored binaries, {} compiled and stored, {} "
                    "failed (BIN-002: unreadable {}, binaries refused {}, pipeline refused {}, "
                    "nothing captured {}, capture failed {}, write failed {}, no key {})",
                    stats.loaded.load(), stats.saved.load(), stats.failed.load(),
                    stats.unreadable.load(), stats.binaries_refused.load(),
                    stats.pipeline_refused.load(), stats.nothing_captured.load(),
                    stats.capture_failed.load(), stats.write_failed.load(), stats.no_key.load());
    }
}

inline VkResult CreateOne(vk::Device device, VkPipelineCache cache,
                          const VkGraphicsPipelineCreateInfo& info, VkPipeline* pipeline) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateGraphicsPipelines(device, cache, 1, &info,
                                                                   nullptr, pipeline);
}

inline VkResult CreateOne(vk::Device device, VkPipelineCache cache,
                          const VkComputePipelineCreateInfo& info, VkPipeline* pipeline) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateComputePipelines(device, cache, 1, &info,
                                                                  nullptr, pipeline);
}

inline vk::ResultValue<vk::Pipeline> Plain(vk::Device device, vk::PipelineCache cache,
                                           const vk::GraphicsPipelineCreateInfo& info) {
    return device.createGraphicsPipeline(cache, info);
}

inline vk::ResultValue<vk::Pipeline> Plain(vk::Device device, vk::PipelineCache cache,
                                           const vk::ComputePipelineCreateInfo& info) {
    return device.createComputePipeline(cache, info);
}

// Whether this create goes through the store at all.
inline bool Usable(const Instance& instance, bool library) {
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    // PERF-GTS-482: with whole pipelines only, libraries are built as before (BIN-003).
    return instance.IsPipelineBinarySupported() && d.vkGetPipelineKeyKHR &&
           d.vkCreatePipelineBinariesKHR && d.vkGetPipelineBinaryDataKHR &&
           !(library && instance.IsPipelineBinaryWholeOnly());
}

// The stored file of a create info; empty when the store is unusable or there is no key.
template <class RawInfo>
std::filesystem::path PathOf(vk::Device device, const RawInfo& raw) {
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    const auto& folder = Folder(device);
    if (folder.empty()) return {};
    VkPipelineCreateInfoKHR key_info{VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR,
                                     const_cast<RawInfo*>(&raw)};
    VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    if (const VkResult key_result = d.vkGetPipelineKeyKHR(device, &key_info, &key);
        key_result != VK_SUCCESS || key.keySize == 0) {
        Note(GetStats().no_key, "no pipeline key", key_result);
        return {};
    }
    return folder / (Hex(key.key, key.keySize) + ".bin");
}

template <class Info>
vk::ResultValue<vk::Pipeline> Create(const Instance& instance, vk::Device device,
                                     vk::PipelineCache cache, const Info& info, bool library) {
    using RawInfo = typename Info::NativeType;
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    const RawInfo& raw = info;
    if (!Usable(instance, library)) {
        // FIX-SGT-192: without binaries, a driver cache file per pipeline where that is on.
        if (PipelineCacheFiles::On(instance)) {
            VkPipeline pipeline = VK_NULL_HANDLE;
            const VkResult result =
                PipelineCacheFiles::Create(instance, device, cache, raw, &pipeline);
            return {vk::Result{result}, vk::Pipeline{pipeline}};
        }
        return Plain(device, cache, info);
    }
    const auto path = PathOf(device, raw);
    if (path.empty()) {
        return Plain(device, cache, info);
    }

    // A stored pipeline: create it from its binaries.
    std::error_code exists_error;
    const bool stored_file = std::filesystem::exists(path, exists_error);
    std::vector<Entry> entries;
    if (stored_file && !Read(path, entries)) {
        Note(GetStats().unreadable, "stored file unreadable", VK_SUCCESS);
    }
    if (stored_file && !entries.empty()) {
        std::vector<VkPipelineBinaryKeyKHR> keys;
        std::vector<VkPipelineBinaryDataKHR> datas;
        for (auto& entry : entries) {
            keys.push_back(entry.key);
            datas.push_back({entry.data.size(), entry.data.data()});
        }
        const VkPipelineBinaryKeysAndDataKHR keys_and_data{static_cast<u32>(entries.size()),
                                                           keys.data(), datas.data()};
        const VkPipelineBinaryCreateInfoKHR create{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,
                                                   nullptr, &keys_and_data, VK_NULL_HANDLE,
                                                   nullptr};
        std::vector<VkPipelineBinaryKHR> binaries(entries.size(), VK_NULL_HANDLE);
        VkPipelineBinaryHandlesInfoKHR handles{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
                                               nullptr, static_cast<u32>(binaries.size()),
                                               binaries.data()};
        const VkResult created = d.vkCreatePipelineBinariesKHR(device, &create, nullptr, &handles);
        if (created != VK_SUCCESS) {
            Note(GetStats().binaries_refused, "driver refused stored binaries", created);
        }
        if (created == VK_SUCCESS) {
            VkPipelineBinaryInfoKHR binary_info{VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR,
                                                raw.pNext, handles.pipelineBinaryCount,
                                                binaries.data()};
            RawInfo from_binaries = raw;
            from_binaries.pNext = &binary_info;
            VkPipeline pipeline = VK_NULL_HANDLE;
            // FIX-SGT-192: no pipeline cache with binaries (VUID-...-pNext-09616); it was passed.
            const VkResult result = CreateOne(device, VK_NULL_HANDLE, from_binaries, &pipeline);
            Destroy(device, binaries);
            if (result != VK_SUCCESS) {
                Note(GetStats().pipeline_refused, "pipeline from stored binaries refused", result);
            }
            if (result == VK_SUCCESS) {
                ++GetStats().loaded;
                if (library) ++GetStats().libraries_loaded;
                Report();
                return {vk::Result::eSuccess, vk::Pipeline{pipeline}};
            }
        } else {
            Destroy(device, binaries);
        }
        // Unusable (a driver change the key did not catch): build again and replace it.
        ++GetStats().failed;
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    // A new pipeline: build it with its data captured, then store its binaries.
    const VkPipelineCreateFlags2CreateInfoKHR flags2{
        VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, raw.pNext,
        static_cast<VkPipelineCreateFlags2KHR>(raw.flags) |
            VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR};
    RawInfo capturing = raw;
    capturing.pNext = &flags2;
    VkPipeline pipeline = VK_NULL_HANDLE;
    // FIX-SGT-192: no pipeline cache with CAPTURE_DATA (VUID-...-pNext-09617). The compile
    // lands in the store, not in the driver cache file the driver keeps whole in memory.
    const VkResult result = CreateOne(device, VK_NULL_HANDLE, capturing, &pipeline);
    if (result != VK_SUCCESS) {
        // Retry without capturing, exactly as before BIN-001.
        Note(GetStats().capture_refused, "capturing create refused", result);
        ++GetStats().failed;
        return Plain(device, cache, info);
    }
    VkPipelineBinaryCreateInfoKHR from_pipeline{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,
                                                nullptr, nullptr, pipeline, nullptr};
    VkPipelineBinaryHandlesInfoKHR handles{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
    std::vector<VkPipelineBinaryKHR> binaries;
    bool stored = false;
    bool captured = false;
    const VkResult counted = d.vkCreatePipelineBinariesKHR(device, &from_pipeline, nullptr, &handles);
    if (counted != VK_SUCCESS || handles.pipelineBinaryCount == 0) {
        Note(GetStats().nothing_captured, "driver gave no binaries", counted);
    }
    if (counted == VK_SUCCESS && handles.pipelineBinaryCount != 0) {
        binaries.assign(handles.pipelineBinaryCount, VK_NULL_HANDLE);
        handles.pPipelineBinaries = binaries.data();
        if (d.vkCreatePipelineBinariesKHR(device, &from_pipeline, nullptr, &handles) ==
            VK_SUCCESS) {
            std::vector<Entry> entries(binaries.size());
            bool complete = true;
            for (size_t i = 0; i < binaries.size() && complete; ++i) {
                const VkPipelineBinaryDataInfoKHR data_info{
                    VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR, nullptr, binaries[i]};
                size_t size = 0;
                complete = d.vkGetPipelineBinaryDataKHR(device, &data_info, &entries[i].key,
                                                        &size, nullptr) == VK_SUCCESS &&
                           size != 0;
                if (complete) {
                    entries[i].data.resize(size);
                    complete = d.vkGetPipelineBinaryDataKHR(device, &data_info, &entries[i].key,
                                                            &size, entries[i].data.data()) ==
                               VK_SUCCESS;
                }
            }
            captured = complete;
            if (!complete) Note(GetStats().capture_failed, "reading binaries out failed", VK_SUCCESS);
            stored = complete && Write(path, entries);
            if (complete && !stored) Note(GetStats().write_failed, "writing binaries failed", VK_SUCCESS);
        }
    }
    Destroy(device, binaries);
    if (d.vkReleaseCapturedPipelineDataKHR) {
        const VkReleaseCapturedPipelineDataInfoKHR release{
            VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR, nullptr, pipeline};
        d.vkReleaseCapturedPipelineDataKHR(device, &release, nullptr);
    }
    ++(stored ? GetStats().saved : GetStats().failed);
    if (stored && library) ++GetStats().libraries_saved;
    Report();
    return {vk::Result::eSuccess, vk::Pipeline{pipeline}};
}

} // namespace detail

// Creates a graphics pipeline (or pipeline library) through the binary store when the device
// has VK_KHR_pipeline_binary, else as before.
inline vk::ResultValue<vk::Pipeline> CreateGraphics(const Instance& instance, vk::Device device,
                                                    vk::PipelineCache cache,
                                                    const vk::GraphicsPipelineCreateInfo& info) {
    const bool library = static_cast<bool>(info.flags & vk::PipelineCreateFlagBits::eLibraryKHR);
    return detail::Create(instance, device, cache, info, library);
}

// FIX-SGT-192: compute pipelines too, when every pipeline goes through the store.
inline vk::ResultValue<vk::Pipeline> CreateCompute(const Instance& instance, vk::Device device,
                                                   vk::PipelineCache cache,
                                                   const vk::ComputePipelineCreateInfo& info) {
    if (instance.IsPipelineBinaryWholeOnly()) return device.createComputePipeline(cache, info);
    return detail::Create(instance, device, cache, info, false);
}

// FIX-SGT-192: a warm-up (a build made only so a later one is quick) has nothing to do when the
// pipeline is stored already; loading it to destroy it would only cost time and memory.
template <class Info>
bool Stored(const Instance& instance, vk::Device device, const Info& info) {
    using RawInfo = typename Info::NativeType;
    const bool library = static_cast<bool>(info.flags & vk::PipelineCreateFlagBits::eLibraryKHR);
    if (!detail::Usable(instance, library)) {
        return PipelineCacheFiles::On(instance) &&
               PipelineCacheFiles::Stored(instance, static_cast<const RawInfo&>(info));
    }
    const auto path = detail::PathOf(device, static_cast<const RawInfo&>(info));
    std::error_code error;
    return !path.empty() && std::filesystem::exists(path, error);
}

// FIX-SGT-192: every pipeline but fast links goes through the store (not AMD's whole-only mode),
// or each gets a driver cache file of its own.
inline bool StoresEverything(const Instance& instance) {
    return (instance.IsPipelineBinarySupported() && !instance.IsPipelineBinaryWholeOnly()) ||
           PipelineCacheFiles::On(instance);
}

// FIX-SGT-192: the totals so far, logged at shutdown.
inline void Summary() {
    PipelineCacheFiles::Summary();
    auto& stats = detail::GetStats();
    if (stats.loaded + stats.saved + stats.failed == 0) return;
    LOG_WARNING(Render_Vulkan,
                "BIN-001: at exit {} pipelines from stored binaries ({} libraries), {} compiled "
                "and stored ({} libraries), {} failed (capture refused {}, binaries refused {}, "
                "pipeline refused {}, nothing captured {}, capture failed {}, write failed {})",
                stats.loaded.load(), stats.libraries_loaded.load(), stats.saved.load(),
                stats.libraries_saved.load(), stats.failed.load(), stats.capture_refused.load(),
                stats.binaries_refused.load(), stats.pipeline_refused.load(),
                stats.nothing_captured.load(), stats.capture_failed.load(),
                stats.write_failed.load());
}

} // namespace Vulkan::PipelineBinaryStore
