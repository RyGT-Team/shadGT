// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string_view>
#include "common/types.h"

namespace Common {

// GAME-001: which Gran Turismo this process runs. One renderer serves GT Sport and GT7, and
// where the two games need different behaviour the choice is made here, once, by one rule,
// instead of by each caller's own test.
enum class GameTitle : u8 {
    GtSport,
    Gt7,
};

// The rule, apart from where its inputs come from. `exe_file` is the host executable's file
// name: the converted game is GTSport.exe or GT7.exe; the title id the Ryty build reports is
// derived from this (GAME-005, ryty_stubs.cpp), so the name is what tells them apart there. The
// title id and name (param.sfo) tell them apart in the standalone emulator: CUSA24767 and
// CUSA24769 are GT7's two regions, other regions go by the name ("Gran Turismo® 7").
constexpr GameTitle GameTitleOf(std::wstring_view exe_file, std::string_view serial,
                                std::string_view name) {
    constexpr std::wstring_view gt7_exe = L"GT7.EXE";
    bool exe_is_gt7 = exe_file.size() == gt7_exe.size();
    for (size_t i = 0; exe_is_gt7 && i < gt7_exe.size(); ++i) {
        const wchar_t c = exe_file[i];
        exe_is_gt7 = (c >= L'a' && c <= L'z' ? wchar_t(c - L'a' + L'A') : c) == gt7_exe[i];
    }
    if (exe_is_gt7 || serial == "CUSA24767" || serial == "CUSA24769" ||
        (name.starts_with("Gran Turismo") && name.ends_with(" 7"))) {
        return GameTitle::Gt7;
    }
    return GameTitle::GtSport;
}

// GAME-004: the game the launcher says it started. The launcher sets SHADGT_GAME (gts or gt7)
// when it starts a game, and that decides; the executable's name is only the fallback for a game
// started without it. Another value is reported and ignored (a wrong value must not make one game
// run as the other).
inline std::optional<GameTitle> LauncherGameTitle() {
    const char* value = std::getenv("SHADGT_GAME");
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    const std::string_view text{value};
    const auto is = [&](std::string_view name) {
        if (text.size() != name.size()) return false;
        for (size_t i = 0; i < name.size(); ++i) {
            const char c = text[i];
            if ((c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c) != name[i]) return false;
        }
        return true;
    };
    if (is("gt7")) return GameTitle::Gt7;
    if (is("gts") || is("gtsport")) return GameTitle::GtSport;
    std::fprintf(stderr, "[game] SHADGT_GAME=%s is not gts or gt7: ignored\n", value);
    return std::nullopt;
}

// The running game, decided at the first call (not before the emulator has read the game).
GameTitle CurrentGameTitle();

inline bool IsGt7() {
    return CurrentGameTitle() == GameTitle::Gt7;
}

// GAME-005: the two games' title ids (US/EU discs the converted games come from).
inline constexpr std::string_view GtSportSerial = "CUSA03220";
inline constexpr std::string_view Gt7Serial = "CUSA24769";

// GAME-005: the running game's title id. Under Ryty it follows the game (GAME-001/003):
// CUSA24769 for GT7, CUSA03220 for GT Sport; it names the shader store, the driver caches and
// the launcher's settings. The standalone emulator gives the one it read from param.sfo.
std::string_view CurrentGameSerial();

// The runtime's /app0 top folder in `root`: app0\<serial> when Set up made one for this game
// (a folder shared by both games), else app0 as before. The runtime picks it the same way.
inline std::filesystem::path App0Folder(const std::filesystem::path& root) {
    std::error_code error;
    const auto own = root / "app0" / std::string(CurrentGameSerial());
    return std::filesystem::is_directory(own, error) ? own : root / "app0";
}

// The game's RyGT.ini in `root`: <serial>.RyGT.ini, so two games in one folder keep their own
// settings; RyGT.ini while a folder has not been moved to the per-game name yet.
inline std::filesystem::path RygtIniPath(const std::filesystem::path& root) {
    std::error_code error;
    const auto own = root / (std::string(CurrentGameSerial()) + ".RyGT.ini");
    return std::filesystem::exists(own, error) ? own : root / "RyGT.ini";
}

// GAME-005: the serial whose user\custom_configs\<serial>.json holds the launcher's settings.
// The game's own, except under GT7 when only CUSA03220.json is there (a GT7 folder from before
// GAME-005 started without the launcher): that one is read, never written. Decided once.
std::string_view GameSettingsSerial();

// GAME-005: GameSettingsSerial() fell back to GT Sport's name under GT7.
bool GameSettingsFromOldName();

// GAME-005: under GT7, renames each <shaders>\CUSA03220* entry to CUSA24769* when nothing of
// the new name is there (GT7's caches from before GAME-005), once, before a cache is opened.
// Never overwrites; never runs under GT Sport, nor when GTSport.exe shares the folder. Logs
// what it moved.
void MigrateGt7ShaderNames();

} // namespace Common
