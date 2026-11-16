// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>
#include "common/elf_info.h"
#include "common/game_title.h"
#include "common/logging/log.h"
#include "common/path_util.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Common {

GameTitle CurrentGameTitle() {
    static const GameTitle title = [] {
        if (const auto launcher = LauncherGameTitle()) { // GAME-004: the launcher says
            return *launcher;
        }
        std::wstring file;
#ifdef _WIN32
        // The longest path Windows gives: a name cut at MAX_PATH would be taken for GT Sport.
        std::wstring path(32768, L'\0');
        path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
        file = path.substr(path.find_last_of(L"\\/") + 1);
#endif
#ifdef RYTY_GPU
        // The title id the Ryty build reports is derived from this (GAME-005, ryty_stubs.cpp):
        // it is not read, which also lets this be asked at any time, before the game's
        // information is filled in (the device and the swapchain ask early).
        return GameTitleOf(file, {}, {});
#else
        const auto& elf = ElfInfo::Instance();
        return GameTitleOf(file, elf.GameSerial(), elf.Title());
#endif
    }();
    return title;
}

std::string_view CurrentGameSerial() {
#ifdef RYTY_GPU
    return IsGt7() ? Gt7Serial : GtSportSerial; // GAME-005: from the game, not the reverse
#else
    return ElfInfo::Instance().GameSerial();
#endif
}

namespace {
std::atomic<bool> g_settings_from_old_name{false};
}

std::string_view GameSettingsSerial() {
#ifdef RYTY_GPU
    static const std::string_view serial = []() -> std::string_view {
        const std::string_view current = CurrentGameSerial();
        if (current != Gt7Serial) {
            return current;
        }
        const auto& dir = FS::GetUserPath(FS::PathType::CustomConfigs);
        std::error_code error;
        const auto ours = dir / (std::string{Gt7Serial} + ".json");
        const auto old = dir / (std::string{GtSportSerial} + ".json");
        if (!std::filesystem::exists(ours, error) && std::filesystem::exists(old, error)) {
            // May be asked in DllMain (the game patches), before the log is set up.
            std::fprintf(stderr,
                         "[game] GAME-005: no %s.json; reading GT7's settings from %s.json\n",
                         Gt7Serial.data(), GtSportSerial.data());
            g_settings_from_old_name = true;
            return GtSportSerial;
        }
        return current;
    }();
    return serial;
#else
    return CurrentGameSerial();
#endif
}

bool GameSettingsFromOldName() {
    (void)GameSettingsSerial();
    return g_settings_from_old_name;
}

void MigrateGt7ShaderNames() {
#ifdef RYTY_GPU
    static std::once_flag once;
    std::call_once(once, [] {
        if (!IsGt7()) {
            return; // GT Sport's caches are CUSA03220's by right
        }
        const auto& dir = FS::GetUserPath(FS::PathType::CacheDir);
        std::error_code error;
        // A folder shared with GT Sport: its CUSA03220 caches are GT Sport's, not GT7's.
        if (std::filesystem::exists(dir.parent_path() / "GTSport.exe", error)) {
            LOG_WARNING(Config, "GAME-005: GTSport.exe is in {}: old shader names left as they are",
                        FS::PathToUTF8String(dir.parent_path()));
            return;
        }
        std::vector<std::filesystem::path> old_entries;
        for (auto it = std::filesystem::directory_iterator(dir, error);
             !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
            const std::string name = it->path().filename().string();
            if (name.starts_with(GtSportSerial)) {
                old_entries.push_back(it->path());
            }
        }
        if (error) {
            LOG_WARNING(Config, "GAME-005: cannot list {}: {}", FS::PathToUTF8String(dir),
                        error.message());
        }
        unsigned moved = 0;
        for (const auto& from : old_entries) {
            const std::string name = from.filename().string();
            const auto to = dir / (std::string{Gt7Serial} + name.substr(GtSportSerial.size()));
            std::error_code exists_error;
            if (std::filesystem::exists(to, exists_error) || exists_error) {
                LOG_INFO(Config, "GAME-005: {} kept: {} is there", name, to.filename().string());
                continue;
            }
            std::error_code rename_error;
            std::filesystem::rename(from, to, rename_error);
            if (rename_error) {
                LOG_WARNING(Config, "GAME-005: cannot rename {} to {}: {}", name,
                            to.filename().string(), rename_error.message());
                continue;
            }
            LOG_WARNING(Config, "GAME-005: GT7 shader cache {} renamed to {}", name,
                        to.filename().string());
            ++moved;
        }
        if (moved != 0) {
            LOG_WARNING(Config, "GAME-005: {} GT7 shader cache entr{} moved to {}", moved,
                        moved == 1 ? "y" : "ies", Gt7Serial);
        }
    });
#endif
}

} // namespace Common
