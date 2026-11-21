// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace ImGui::TrophyToast {

void Register();
void Unregister();

void Push(std::string name, std::string icon_path_utf8, char grade);

} // namespace ImGui::TrophyToast
