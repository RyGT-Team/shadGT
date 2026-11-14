// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// SWITCH-001: the renderer's switches were named RYGT_* in shadgt7 and are SHADGT_* (or
// SHARED_*) here (FEAT-GTS-415). ryGT7's launcher, its settings file and the players' own
// launch scripts still set the old names, and a switch that is silently not read is worse than
// one that fails: the game would start with other settings than the ones chosen. So an old
// name that is set, while its new name is not, is read as the new one. The new name wins when
// both are set. SHADGT_OLD_SWITCH_NAMES=0: only the new names are read.

#include <cstdlib>
#include <cstring>
#include <string>
#include "common/old_switch_names.h"

#ifdef _WIN32
#include <windows.h>

// Switches are read by static initializers all over this library; this one must run first.
#pragma warning(disable : 4073)
#pragma init_seg(lib)
#endif

namespace Common {

namespace {

std::string& Note() {
    static std::string note;
    return note;
}

#ifdef _WIN32
struct OldSwitchNames {
    OldSwitchNames() {
        if (const char* off = std::getenv("SHADGT_OLD_SWITCH_NAMES"); off && off[0] == '0') {
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
