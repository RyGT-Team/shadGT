// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// PERF-SGT-026: hash functors for the sets and maps the command processor looks into for every
// draw. MSVC's std::hash runs FNV-1a over the key's bytes, and std::unordered_set allocates a
// node per insert; with tsl::robin_set/robin_map and these, a lookup is a mix and a probe.

#include <cstddef>
#include <cstdint>

namespace Common {

/// For keys that are already hashes (XXH3 of a program's code or of a draw's state).
struct IdentityHash {
    std::size_t operator()(std::uint64_t value) const noexcept {
        return static_cast<std::size_t>(value);
    }
};

/// For pointers: a 64-bit mix (the low bits of an address repeat with its alignment).
struct PointerHash {
    template <typename T>
    std::size_t operator()(const T* pointer) const noexcept {
        std::uint64_t value = reinterpret_cast<std::uintptr_t>(pointer);
        value ^= value >> 33;
        value *= 0xff51afd7ed558ccdull;
        value ^= value >> 33;
        return static_cast<std::size_t>(value);
    }
};

} // namespace Common
