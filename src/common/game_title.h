// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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
// name: the converted game is GTSport.exe or GT7.exe, and the title id the Ryty build reports
// is GT Sport's for both (ryty_stubs.cpp), so the name is what tells them apart there. The
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

// The running game, decided at the first call (not before the emulator has read the game).
GameTitle CurrentGameTitle();

inline bool IsGt7() {
    return CurrentGameTitle() == GameTitle::Gt7;
}

} // namespace Common
