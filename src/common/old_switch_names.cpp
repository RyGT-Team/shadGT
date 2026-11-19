// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// SWITCH-001: the renderer's switches were named RYGT_* in shadgt7 and are SHADGT_* (or
// SHARED_*) here (FEAT-GTS-415). ryGT7's launcher, its settings file and the players' own
// launch scripts still set the old names, and a switch that is silently not read is worse than
// one that fails: the game would start with other settings than the ones chosen. So in GT7 an
// old name that is set, while its new name is not, is read as the new one. The new name wins
// when both are set. SHADGT_OLD_SWITCH_NAMES=0: only the new names are read.
// GT Sport is left alone: its launcher and settings use the new names already, and a RYGT_*
// variable set for GT7 in the user's environment must not reach it.

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include "common/game_title.h"
#include "common/old_switch_names.h"

#ifdef _WIN32
#include <windows.h>

// Switches are read by static initializers all over this library; this one must run first.
#pragma warning(disable : 4073)
#pragma init_seg(lib)
#endif

namespace Common {

namespace {

// Read by ryGT7's runtime alone (tools/switch_prefix.py, RUNTIME['gt7']): these never were
// the renderer's, so they get no twin (RYGT_PHOTO_EXPORT would otherwise turn on GT Sport's
// photo export, another feature under the same suffix).
constexpr std::array<std::string_view, 26> RuntimeOnly = {
    "APP0_LAYERS",          "AVP_TRACE",           "BATCHMAP_MERGE",
    "COND_TARGETED",        "DEFER_UNMAP",         "DIAG_BATCHMAP",
    "DIAG_DEPENDENCIES",    "DIAG_MAPPING_FILE",   "FAST_NATIVE_MAPPINGS",
    "FIBER_HOST_STACKS",    "HDR_DISPLAY_NITS",    "JPEG_SUBSAMPLED_CAP",
    "LOG_DIRECTORY",        "MAPPING_HANDLE_REUSE", "MAPPING_QUERY_REUSE",
    "MAPPING_VIEW_BOUNDS",  "MAPPING_VIEW_CHUNK",  "MOVIES",
    "ONLINE",               "ONLINE_ADDRESS",      "ONLINE_ID",
    "PCORES",               "PHOTO_EXPORT",        "PLACEHOLDER_CACHE",
    "STAGED_GUEST_READS",   "USER_NAME",
};

std::string& Note() {
    static std::string note;
    return note;
}

#ifdef _WIN32
bool RunningGt7() {
    if (const auto launcher = LauncherGameTitle()) { // GAME-004: the launcher says
        return *launcher == GameTitle::Gt7;
    }
    // The executable's name (GAME-001): nothing else is known this early.
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    const std::wstring_view file =
        std::wstring_view{path}.substr(path.find_last_of(L"\\/") + 1);
    return GameTitleOf(file, {}, {}) == GameTitle::Gt7;
}

struct OldSwitchNames {
    OldSwitchNames() {
        if (const char* off = std::getenv("SHADGT_OLD_SWITCH_NAMES"); off && off[0] == '0') {
            return;
        }
        if (!RunningGt7()) {
            return;
        }
        char* const block = GetEnvironmentStringsA();
        if (!block) {
            return;
        }
        constexpr char old_prefix[] = "RYGT_";
        constexpr size_t old_size = sizeof(old_prefix) - 1;
        for (const char* entry = block; *entry; entry += std::strlen(entry) + 1) {
            const char* const equals = std::strchr(entry, '=');
            if (std::strncmp(entry, old_prefix, old_size) != 0 || !equals ||
                equals == entry + old_size) {
                continue;
            }
            const std::string suffix{entry + old_size, equals};
            bool runtime_only = false;
            for (const std::string_view name : RuntimeOnly) {
                runtime_only = runtime_only || name == suffix;
            }
            if (runtime_only) {
                continue;
            }
            bool taken = false;
            for (const char* prefix : {"SHADGT_", "SHARED_"}) {
                const std::string name = prefix + suffix;
                if (std::getenv(name.c_str()) == nullptr &&
                    _putenv_s(name.c_str(), equals + 1) == 0) {
                    taken = true;
                }
            }
            if (taken) {
                auto& note = Note();
                note += note.empty() ? "" : ", ";
                note += old_prefix + suffix;
            }
        }
        FreeEnvironmentStringsA(block);
    }
} g_old_switch_names;
#endif

} // namespace

const std::string& OldSwitchNamesTaken() {
    return Note();
}

} // namespace Common
