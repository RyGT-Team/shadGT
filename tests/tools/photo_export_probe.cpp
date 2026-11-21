// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include "common/game_title.h"

void InstallPhotoExport();
bool ConfigFlag(const char*, bool fallback) {
    return fallback;
}

std::string_view Common::CurrentGameSerial() {
    return "TEST00001";
}

int main() {
    InstallPhotoExport();
    std::getchar(); // The test driver closes stdin after inspecting the exported images.
}
