// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include "common/elf_info.h"
#include "common/game_title.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Common {

GameTitle CurrentGameTitle() {
    static const GameTitle title = [] {
        std::wstring file;
#ifdef _WIN32
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring path{exe};
        file = path.substr(path.find_last_of(L"\\/") + 1);
#endif
        const auto& elf = ElfInfo::Instance();
        return GameTitleOf(file, elf.GameSerial(), elf.Title());
    }();
    return title;
}

} // namespace Common
