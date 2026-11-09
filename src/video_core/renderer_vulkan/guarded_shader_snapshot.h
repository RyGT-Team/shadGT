// Copyright 2026 RyGT team. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>
#include "shader_recompiler/params.h"

namespace Vulkan {
struct GuardedShaderSnapshot {
    std::uintptr_t guest_base{};
    std::uint64_t hash{};
    std::vector<std::uint32_t> code;
};
inline Shader::ShaderParams MakeOwnedShaderParams(
    std::span<const u32, Shader::ShaderParams::NumShaderUserData> user_data,
    const GuardedShaderSnapshot& snapshot) {
    return {.user_data = user_data, .code = snapshot.code, .hash = snapshot.hash,
            .source_base = snapshot.guest_base};
}

// Read is a fault-guarded copy of guest bytes. BinaryInfo is the engine's real
// metadata type. Re-read metadata after copying code so an observed metadata
// change cannot certify a different program. This is not an atomic guest snapshot.
template <typename BinaryInfo, typename Read>
std::optional<GuardedShaderSnapshot> CaptureShaderSnapshot(std::uintptr_t base, Read&& read,
                                                          std::size_t code_limit = 16 * 1024 * 1024) {
    constexpr std::uint32_t marker = 0xBEEB03FF;
    constexpr std::size_t scan_words = 0x4000;
    constexpr std::size_t max_bytes = 16 * 1024 * 1024;
    if (!base || base > std::numeric_limits<std::uintptr_t>::max() - max_bytes) return {};
    std::array<std::uint32_t, 2> prefix{};
    if (!read(prefix.data(), base, sizeof(prefix))) return {};
    BinaryInfo info{};
    std::size_t found = std::numeric_limits<std::size_t>::max();
    const auto examine = [&](std::size_t offset) {
        if (offset > max_bytes - sizeof(info)) return false;
        return read(&info, base + offset, sizeof(info)) && info.Valid();
    };
    if (prefix[0] == marker) {
        const std::uint64_t offset = (std::uint64_t(prefix[1]) + 1) * 8;
        if (offset < max_bytes && examine(static_cast<std::size_t>(offset))) found = offset;
    }
    if (found == std::numeric_limits<std::size_t>::max()) {
        for (std::size_t word = 0; word < scan_words; ++word) {
            const auto offset = word * sizeof(std::uint32_t);
            if (examine(offset)) { found = offset; break; }
        }
    }
    const std::size_t bytes = info.length;
    if (found == std::numeric_limits<std::size_t>::max() || bytes == 0 || bytes > max_bytes || bytes > code_limit ||
        bytes % 4) return {};
    GuardedShaderSnapshot snapshot{.guest_base = base, .hash = info.shader_hash};
    snapshot.code.resize(bytes / sizeof(std::uint32_t));
    if (!read(snapshot.code.data(), base, bytes)) return {};
    BinaryInfo captured{};
    if (!read(&captured, base + found, sizeof(captured)) ||
        std::memcmp(&captured, &info, sizeof(info)) != 0) return {};
    return snapshot;
}
} // namespace Vulkan
