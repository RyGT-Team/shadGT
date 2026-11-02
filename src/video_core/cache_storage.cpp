// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/buffer_cache/direct_memory.h"
#include "video_core/cache_storage.h"
#include "video_core/deferred_task_queue.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>

namespace {

Storage::DeferredTaskQueue<std::packaged_task<void()>> requests;

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadGT:PipelineCacheIO");

    while (auto request = requests.Pop(stoken)) {
        if (request->valid()) (*request)();
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_zero_struct(&zip_ar);

        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{game_info.GameSerial()}.replace_extension(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else {
        cache_path = GetUserPath(PathType::CacheDir) / game_info.GameSerial();
        // D2: direct-memory shaders differ from the stored ones, which load unchecked.
        if (VideoCore::DirectMemory::Enabled()) {
            // SHD-002: stage descriptor sets: translations of their own.
            cache_path += "-direct-sets";
        }
        // FIX-213: stored translations load unchecked, so after an update that changes how
        // shaders are translated, the store kept the old translations and drew with them (a
        // magenta car and red scenery on an RX 7600 after updating to PR #88, until the folder
        // was deleted by hand). The store now carries a version: a store without this one is
        // deleted and starts again, along with any previous <store>.outdated backup. The
        // launcher's shader pack stamp is removed, so the next Play prepares the new store. Raise the
        // version whenever a change makes stored translations wrong.
        // AMD-311: each vertex order of AMD's per-vertex reads has a store of its own version.
        // AMD-312: Radeon RX 9000 cards (RDNA4) use store 4: an RX 9060 XT drew the road at
        // Brands Hatch as streaks with a store made earlier and right once it was translated
        // again. Other GPUs (the RX 7000 series) keep store 3, so their store stays in use.
        const char* order_env = std::getenv("SHADGT_AMD_VERTEX_ORDER");
        // AMD-313: RX 7000 cards leave FIX-215/216 out (purple car); their store 5 sets aside
        // stores translated with them.
        const auto family = Vulkan::GetStoreGpuFamily();
        const std::string StoreVersion =
            std::string{family == Vulkan::StoreGpuFamily::AmdRdna4
                            ? "RyGT shader store 4 (FIX-215/216 NaN on AMD, AMD-311 vertex order)"
                        : family == Vulkan::StoreGpuFamily::AmdRdna3
                            ? "RyGT shader store 5 (RX 7000: FIX-215/216 off, AMD-313)"
                        : family == Vulkan::StoreGpuFamily::AmdLegacy
                            ? "RyGT shader store 8 (RX 6000 to RX 400: AMD-306/307/308/315, order "
                              "check gated)"
                            : "RyGT shader store 3 (FIX-215/216 MED3, MIN3, MAX3 NaN on AMD)"} +
            (order_env && *order_env ? fmt::format(", SHADGT_AMD_VERTEX_ORDER={}", order_env[0])
                                     : std::string{}) +
            [] {
                // AMD-313, AMD-306, AMD-307, AMD-308: the switches that change translations.
                std::string parts;
                for (const char* name :
                     {"SHADGT_AMD_NAN_RULES", "SHADGT_QUAD_CONTROL", "SHADGT_VERTEX_ORDER",
                      "SHADGT_FP32_IMAGE_ATOMICS", "SHADGT_MANUAL_INTERP"}) {
                    if (const char* env = std::getenv(name); env && *env) {
                        parts += fmt::format(", {}={}", name, env[0]);
                    }
                }
                return parts;
            }();
        const auto version_path = cache_path / "store-version.txt";
        std::error_code ec;
        if (std::filesystem::exists(cache_path, ec)) {
            std::string version;
            if (std::ifstream file{version_path}; file) {
                std::getline(file, version);
            }
            if (version != StoreVersion) {
                auto outdated = cache_path;
                outdated += ".outdated";
                std::filesystem::remove_all(outdated, ec);
                if (ec) {
                    throw std::runtime_error("Could not remove obsolete shader cache: " + ec.message());
                }
                std::filesystem::remove_all(cache_path, ec);
                if (ec) {
                    throw std::runtime_error("Could not remove incompatible shader cache: " + ec.message());
                } else {
                    std::filesystem::remove(GetUserPath(PathType::CacheDir) / "shaders-pack.txt",
                                            ec);
                    LOG_WARNING(Render, "FIX-213: shader store of another version ('{}') removed; "
                                "shaders are translated again", version);
                }
            }
        }
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
        if (!std::filesystem::exists(version_path, ec)) {
            std::ofstream{version_path} << StoreVersion << '\n';
        }
    }

    io_worker = std::jthread{ProcessIO};
    opened = true;
}

void DataBase::OpenLegacy() {
    if (opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    const auto& game_info = Common::ElfInfo::Instance();
    cache_path = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / game_info.GameSerial();
    std::error_code ec;
    opened = std::filesystem::is_directory(cache_path, ec);
    LOG_INFO(Render, "D10: old renderer's shader store {} at {}", opened ? "found" : "not found",
             cache_path.string());
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    io_worker.request_stop();
    io_worker.join();

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    {
        auto request = std::packaged_task<void()>{[type, path=std::move(path_), data=std::move(v)]() mutable {
            path.replace_extension(GetBlobFileExtension(type));
            if (EmulatorSettings.IsPipelineCacheArchived()) {
                ASSERT_MSG(!ar_is_read_only,
                           "The archive is read-only. Did you forget to call `FinishPreload`?");
                if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), data.data(),
                                           data.size() * sizeof(T), MZ_BEST_COMPRESSION)) {
                    LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
                }
            } else {
                using namespace Common::FS;
                const auto file = IOFile{path, FileAccessMode::Create};
                file.Write(data);
            }
        }};
        requests.Push(std::move(request));
    }

    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        int index{-1};
        index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            LOG_WARNING(Render, "File {} is not found in the archive", path.string().c_str());
            return;
        }
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&zip_ar, index, &stat);
        v.resize(stat.m_uncomp_size / sizeof(T));
        mz_zip_reader_extract_to_mem(&zip_ar, index, v.data(), stat.m_uncomp_size, 0);
    } else {
        const auto file = IOFile{path, FileAccessMode::Read};
        v.resize(file.GetSize() / sizeof(T));
        file.Read(v);
    }
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

bool DataBase::Exists(BlobType type, const std::string& name) const {
    if (!opened) {
        return false;
    }
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        std::filesystem::path path{name};
        path.replace_extension(GetBlobFileExtension(type));
        return mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0) >= 0;
    }
    auto path = cache_path / name;
    path.replace_extension(GetBlobFileExtension(type));
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            file_name.fill(0);
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            if (std::string{file_name.data()}.ends_with(ext)) {
                mz_zip_archive_file_stat stat{};
                mz_zip_reader_file_stat(&zip_ar, index, &stat);
                std::vector<u8> data(stat.m_uncomp_size);
                mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0);
                func(std::move(data));
            }
        }
    } else {
        for (const auto& file_name : std::filesystem::directory_iterator{cache_path}) {
            if (file_name.path().extension().string().ends_with(ext)) {
                using namespace Common::FS;
                const auto& file = IOFile{file_name, FileAccessMode::Read};
                if (file.IsOpen()) {
                    std::vector<u8> data(file.GetSize());
                    file.Read(data);
                    func(std::move(data));
                }
            }
        }
    }
}

std::vector<std::pair<std::string, std::filesystem::file_time_type>> DataBase::ListBlobs(
    BlobType type) const {
    std::vector<std::pair<std::string, std::filesystem::file_time_type>> blobs;
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return blobs;
    }
    const auto ext = "." + GetBlobFileExtension(type);
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{cache_path, ec}) {
        const auto& path = entry.path();
        if (path.extension().string() != ext) {
            continue;
        }
        std::error_code time_ec;
        const auto time = entry.last_write_time(time_ec);
        blobs.emplace_back(path.stem().string(), time_ec ? std::filesystem::file_time_type{} : time);
    }
    return blobs;
}

void DataBase::FinishPreload() {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

} // namespace Storage
