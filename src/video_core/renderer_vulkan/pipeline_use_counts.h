// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// FIX-SGT-192: how many sessions used each stored graphics pipeline, kept across runs in
// <cache dir>/<serial>.pipeline_uses. The precompile builds the most used first, so the pipelines
// its keep (SHADGT_PRECOMPILE_KEEP, 2048) holds are the ones played most, not only the most recent;
// the trim under memory pressure releases the least used first. With a cache file per pipeline,
// a released pipeline is built again from its file, which costs a few ms more than a hit in the
// one in-memory driver cache did, so the pipelines that come back most should be kept.

#pragma once

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/elf_info.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

namespace Vulkan::PipelineUses {

namespace detail {

inline constexpr u32 Magic = 0x31555050; // "PPU1"

struct Table {
    std::mutex mutex;
    bool loaded = false;
    bool dirty = false;
    std::unordered_map<u64, u32> sessions;  // key hash -> sessions that used it
    std::unordered_set<u64> this_session;
    std::chrono::steady_clock::time_point saved_at = std::chrono::steady_clock::now();
};

inline Table& Get() {
    static Table table;
    return table;
}

inline std::filesystem::path Path() {
    const auto& serial = Common::Singleton<Common::ElfInfo>::Instance()->GameSerial();
    return Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
           (std::string{serial} + ".pipeline_uses");
}

// File: u32 "PPU1", u32 count, then per pipeline u64 key hash and u32 sessions.
inline void LoadLocked(Table& t) {
    if (t.loaded) return;
    t.loaded = true;
    std::ifstream file{Path(), std::ios::binary};
    u32 magic = 0, count = 0;
    if (!file || !file.read(reinterpret_cast<char*>(&magic), 4) || magic != Magic ||
        !file.read(reinterpret_cast<char*>(&count), 4) || count > (1u << 24)) {
        return;
    }
    t.sessions.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u64 hash = 0;
        u32 uses = 0;
        if (!file.read(reinterpret_cast<char*>(&hash), 8) ||
            !file.read(reinterpret_cast<char*>(&uses), 4)) {
            break;
        }
        t.sessions[hash] = uses;
    }
}

inline std::vector<u8> SnapshotLocked(const Table& t) {
    std::vector<u8> bytes(8 + t.sessions.size() * 12);
    const u32 magic = Magic;
    const u32 count = static_cast<u32>(t.sessions.size());
    std::memcpy(bytes.data(), &magic, 4);
    std::memcpy(bytes.data() + 4, &count, 4);
    std::size_t at = 8;
    for (const auto& [hash, uses] : t.sessions) {
        std::memcpy(bytes.data() + at, &hash, 8);
        std::memcpy(bytes.data() + at + 8, &uses, 4);
        at += 12;
    }
    return bytes;
}

inline void Write(const std::vector<u8>& bytes) {
    const auto path = Path();
    auto temp = path;
    // A temporary of this thread's own: the periodic save and the one at exit may overlap.
    temp += ".";
    temp += std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    temp += ".tmp";
    {
        std::ofstream file{temp, std::ios::binary | std::ios::trunc};
        if (!file) return;
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!file) return;
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    if (error) std::filesystem::remove(temp, error);
}

} // namespace detail

/// Sessions that used the pipeline with this key hash (this one included once it was used).
inline u32 Count(u64 hash) {
    auto& t = detail::Get();
    std::scoped_lock lock{t.mutex};
    detail::LoadLocked(t);
    const auto it = t.sessions.find(hash);
    return it != t.sessions.end() ? it->second : 0;
}

/// Writes the table now (at exit).
inline void Save() {
    auto& t = detail::Get();
    std::vector<u8> bytes;
    {
        std::scoped_lock lock{t.mutex};
        if (!t.dirty) return;
        t.dirty = false;
        t.saved_at = std::chrono::steady_clock::now();
        bytes = detail::SnapshotLocked(t);
    }
    detail::Write(bytes);
}

/// Reads the table now (at the pipeline cache's start), so no draw waits for the file.
inline void Preload() {
    auto& t = detail::Get();
    std::scoped_lock lock{t.mutex};
    detail::LoadLocked(t);
}

/// A draw used this pipeline: counted once a session. Saved about every two minutes; the copy
/// and the write are made on a thread of their own, so the caller only starts it.
inline void NoteUse(u64 hash) {
    auto& t = detail::Get();
    {
        std::scoped_lock lock{t.mutex};
        detail::LoadLocked(t);
        if (!t.this_session.insert(hash).second) return;
        ++t.sessions[hash];
        t.dirty = true;
        const auto now = std::chrono::steady_clock::now();
        if (now - t.saved_at < std::chrono::minutes{2}) return;
        t.saved_at = now; // no other save starts meanwhile
    }
    std::thread([] { Save(); }).detach();
}

} // namespace Vulkan::PipelineUses
