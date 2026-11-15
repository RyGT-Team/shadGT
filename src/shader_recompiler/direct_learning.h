// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// PERF-GTS-479: which direct-eligible buffers of which shaders draws ever read in place (D3).
// A buffer a shader's draws never read in place is translated without the direct-memory path
// (most of a fragment shader's driver compile, measured in DIAG-GTS-477): its draws bind it as an
// SSBO, which is always right, so a wrong guess costs a copy, never a wrong picture.
//
// The file <shaders>/<serial>.direct-reads holds one line per shader: hash, binds seen over all
// runs, mask of buffer slots read in place at least once. It is read once at start, so every
// translation of a run decides the same way; the run's binds are merged in and saved every
// minute and at exit. Decisions use only what earlier runs saw.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>

#include "common/types.h"

namespace Shader::DirectLearning {

/// Binds a shader needs over all runs before its never-read-in-place buffers lose the path.
constexpr u64 MinBinds = 32;
/// Buffer slots tracked (slot index in Info::buffers); later slots keep the direct path.
constexpr u32 MaxSlots = 64;

struct Entry {
    u64 binds{};
    u64 in_place{}; // slot mask
};

struct State {
    std::mutex mutex;
    std::filesystem::path path;
    bool loaded{};
    std::unordered_map<u64, Entry> start;   // read at start, never changed (decisions)
    std::unordered_map<u64, Entry> session; // this run's binds only, merged in on save
    bool dirty{};
    std::chrono::steady_clock::time_point last_save{};
    std::atomic<u64> no_direct_slots{}; // decisions made this run (log)
};

inline State& GetState() {
    static State state;
    return state;
}

/// SHADGT_LEARNED_DIRECT=0 keeps every direct path (and stops learning).
inline bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADGT_LEARNED_DIRECT");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

inline std::unordered_map<u64, Entry> ReadFile(const std::filesystem::path& path) {
    std::unordered_map<u64, Entry> entries;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        char* end{};
        const u64 hash = std::strtoull(line.c_str(), &end, 16);
        const u64 binds = std::strtoull(end, &end, 10);
        const u64 mask = std::strtoull(end, &end, 16);
        if (hash != 0) {
            auto& entry = entries[hash];
            entry.binds = std::max(entry.binds, binds);
            entry.in_place |= mask;
        }
    }
    return entries;
}

/// Reads the learned file. Call once at start, before any translation.
inline size_t Load(const std::filesystem::path& path) {
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.path = path;
    state.loaded = true;
    if (Enabled()) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            state.start = ReadFile(path);
        }
    }
    return state.start.size();
}

/// Whether slot `slot` of shader `hash` is translated without the direct-memory path.
inline bool NoDirect(u64 hash, u32 slot) {
    auto& state = GetState();
    if (!Enabled() || !state.loaded || slot >= MaxSlots) {
        return false;
    }
    const auto it = state.start.find(hash); // never written after Load
    return it != state.start.end() && it->second.binds >= MinBinds &&
           !((it->second.in_place >> slot) & 1);
}

inline void SaveLocked(State& state) {
    if (state.path.empty() || !state.dirty) {
        return;
    }
    // Merge with what is on disk now (another run of the game may have written it).
    auto merged = ReadFile(state.path);
    for (const auto& [hash, entry] : state.start) {
        auto& out = merged[hash];
        out.binds = std::max(out.binds, entry.binds);
        out.in_place |= entry.in_place;
    }
    for (const auto& [hash, entry] : state.session) { // binds: this run's only
        const auto start = state.start.find(hash);
        const u64 earlier = start != state.start.end() ? start->second.binds : 0;
        auto& out = merged[hash];
        out.binds = std::max(out.binds, earlier + entry.binds);
        out.in_place |= entry.in_place;
    }
    const auto tmp = std::filesystem::path(state.path).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            return;
        }
        out << "# PERF-GTS-479: shader hash, binds seen, buffer slots read in place (hex mask)\n";
        for (const auto& [hash, entry] : merged) {
            char line[64];
            std::snprintf(line, sizeof(line), "%016llx %llu %llx\n",
                          static_cast<unsigned long long>(hash),
                          static_cast<unsigned long long>(entry.binds),
                          static_cast<unsigned long long>(entry.in_place));
            out << line;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, state.path, ec);
    if (!ec) {
        state.dirty = false;
    }
}

inline void Save() {
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    SaveLocked(state);
}

/// A draw bound `entries` (the per-buffer direct-table words, mode in [3]) for shader `hash`;
/// `candidate` says which slots may be read in place. Cheap: each thread keeps its own counts and
/// takes the lock only at a bind milestone or when a slot is first read in place.
template <typename CandidateFn>
inline void NoteBind(u64 hash, std::span<const std::array<u32, 4>> entries,
                     CandidateFn&& candidate) {
    if (!Enabled()) {
        return;
    }
    struct Local {
        u64 binds{};
        u64 in_place{};
        u64 reported_binds{};
        u64 reported_in_place{};
    };
    thread_local std::unordered_map<u64, Local> local;
    auto& mine = local[hash];
    ++mine.binds;
    const u32 count = static_cast<u32>(std::min<size_t>(entries.size(), MaxSlots));
    for (u32 i = 0; i < count; ++i) {
        // Mode 1 reads in place; >1 (compute SSBO prefix) still reads past the prefix in place.
        if (entries[i][3] != 0 && candidate(i)) {
            mine.in_place |= u64{1} << i;
        }
    }
    // Powers of two, then every 4096: a thread's count is reported to within half, however the
    // binds of a shader are spread over the recorder threads.
    const bool milestone =
        (mine.binds & (mine.binds - 1)) == 0 || mine.binds % 4096 == 0;
    if (!milestone && mine.in_place == mine.reported_in_place) {
        return;
    }
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    auto& entry = state.session[hash];
    entry.binds += mine.binds - mine.reported_binds;
    entry.in_place |= mine.in_place;
    mine.reported_binds = mine.binds;
    mine.reported_in_place = mine.in_place;
    state.dirty = true;
    const auto now = std::chrono::steady_clock::now();
    if (now - state.last_save >= std::chrono::minutes{1}) {
        state.last_save = now;
        SaveLocked(state);
    }
}

} // namespace Shader::DirectLearning
