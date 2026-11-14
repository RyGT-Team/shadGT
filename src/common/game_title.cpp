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
        // The longest path Windows gives: a name cut at MAX_PATH would be taken for GT Sport.
        std::wstring path(32768, L'\0');
        path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
        file = path.substr(path.find_last_of(L"\\/") + 1);
#endif
#ifdef RYTY_GPU
        // The title id the Ryty build reports is GT Sport's for both games (ryty_stubs.cpp):
        // it says nothing, and not reading it lets this be asked at any time, before the
        // game's information is filled in too (the device and the swapchain ask early).
        return GameTitleOf(file, {}, {});
#else
        const auto& elf = ElfInfo::Instance();
        return GameTitleOf(file, elf.GameSerial(), elf.Title());
#endif
    }();
    return title;
}

} // namespace Common
