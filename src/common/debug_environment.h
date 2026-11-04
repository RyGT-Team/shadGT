// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifndef NDEBUG
#include <cstdlib>
#endif

namespace Common {
// Only diagnostic options use this accessor. Functional settings keep their
// normal configuration path in every build.
#ifdef NDEBUG
inline constexpr const char* DebugEnv(const char*) noexcept { return nullptr; }
#else
inline const char* DebugEnv(const char* name) noexcept { return std::getenv(name); }
#endif
} // namespace Common
