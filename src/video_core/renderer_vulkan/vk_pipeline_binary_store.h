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
#include <vector>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_instance.h"

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

// File: u32 count, then per binary: u32 key size, key bytes, u64 data size, data bytes.
inline bool Read(const std::filesystem::path& path, std::vector<Entry>& entries) {
    std::ifstream file{path, std::ios::binary};
    if (!file) return false;
    u32 count = 0;
    if (!file.read(reinterpret_cast<char*>(&count), sizeof(count)) || count == 0 || count > 64)
        return false;
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
        if (!file.read(reinterpret_cast<char*>(entry.data.data()), data_size)) return false;
    }
    return true;
}

inline bool Write(const std::filesystem::path& path, const std::vector<Entry>& entries) {
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream file{temp, std::ios::binary | std::ios::trunc};
        if (!file) return false;
        const u32 count = static_cast<u32>(entries.size());
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));
        for (const auto& entry : entries) {
            const u32 key_size = entry.key.keySize;
            const u64 data_size = entry.data.size();
            file.write(reinterpret_cast<const char*>(&key_size), sizeof(key_size));
            file.write(reinterpret_cast<const char*>(entry.key.key), key_size);
            file.write(reinterpret_cast<const char*>(&data_size), sizeof(data_size));
            file.write(reinterpret_cast<const char*>(entry.data.data()), data_size);
        }
        if (!file) return false;
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    if (error) std::filesystem::remove(temp, error);
    return !error;
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

} // namespace detail

// Creates a graphics pipeline (or pipeline library) through the binary store when the device
// has VK_KHR_pipeline_binary, else as before.
inline vk::ResultValue<vk::Pipeline> CreateGraphics(const Instance& instance, vk::Device device,
                                                    vk::PipelineCache cache,
                                                    const vk::GraphicsPipelineCreateInfo& info) {
    using namespace detail;
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    if (!instance.IsPipelineBinarySupported() || !d.vkGetPipelineKeyKHR ||
        !d.vkCreatePipelineBinariesKHR || !d.vkGetPipelineBinaryDataKHR) {
        return device.createGraphicsPipeline(cache, info);
    }
    const auto& folder = Folder(device);
    if (folder.empty()) {
        return device.createGraphicsPipeline(cache, info);
    }
    const VkGraphicsPipelineCreateInfo& raw = info;
    VkPipelineCreateInfoKHR key_info{VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR,
                                     const_cast<VkGraphicsPipelineCreateInfo*>(&raw)};
    VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    if (const VkResult key_result = d.vkGetPipelineKeyKHR(device, &key_info, &key);
        key_result != VK_SUCCESS || key.keySize == 0) {
        Note(GetStats().no_key, "no pipeline key", key_result);
        return device.createGraphicsPipeline(cache, info);
    }
    const auto path = folder / (Hex(key.key, key.keySize) + ".bin");

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
            VkGraphicsPipelineCreateInfo from_binaries = raw;
            from_binaries.pNext = &binary_info;
            VkPipeline pipeline = VK_NULL_HANDLE;
            const VkResult result = d.vkCreateGraphicsPipelines(device, cache, 1, &from_binaries,
                                                                nullptr, &pipeline);
            Destroy(device, binaries);
            if (result != VK_SUCCESS) {
                Note(GetStats().pipeline_refused, "pipeline from stored binaries refused", result);
            }
            if (result == VK_SUCCESS) {
                ++GetStats().loaded;
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
    VkGraphicsPipelineCreateInfo capturing = raw;
    capturing.pNext = &flags2;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result =
        d.vkCreateGraphicsPipelines(device, cache, 1, &capturing, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        // Retry without capturing, exactly as before BIN-001.
        return device.createGraphicsPipeline(cache, info);
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
    Report();
    return {vk::Result::eSuccess, vk::Pipeline{pipeline}};
}

} // namespace Vulkan::PipelineBinaryStore
