// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdlib>

namespace Common {
#ifdef SHADGT_DIAGNOSTICS
inline constexpr bool DiagnosticsBuilt = true;
inline const char* DiagEnv(const char* name) { return std::getenv(name); }
#else
inline constexpr bool DiagnosticsBuilt = false;
constexpr const char* DiagEnv(const char*) { return nullptr; }
#endif
} // namespace Common
