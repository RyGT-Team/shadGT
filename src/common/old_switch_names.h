// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace Common {

// SWITCH-001: the old RYGT_* switch names found set at startup and read as their new names
// (old_switch_names.cpp), comma separated; empty when there were none.
const std::string& OldSwitchNamesTaken();

} // namespace Common
