// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace VideoCore {
inline std::filesystem::path ShaderStoreMarksPath(const std::filesystem::path& cache_root,
    std::string_view serial, const std::filesystem::path& store, bool sibling) {
    return sibling ? store / "else-scope-fix" : cache_root / (std::string(serial) + ".else-scope-fix");
}
// An existing store with unknown or incompatible translation metadata stays untouched.
inline std::optional<std::filesystem::path> OpenCompatibleShaderStore(
    const std::filesystem::path& original, std::string_view expected) {
    for (unsigned suffix = 0; suffix != 1024; ++suffix) {
        auto path = original;
        if (suffix) path += ".v" + std::to_string(suffix);
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (ec) return {};
        if (exists) {
            std::ifstream file(path / "store-version.txt");
            std::string version;
            if (file && std::getline(file, version) && version == expected) return path;
            continue;
        }
        if (!std::filesystem::create_directories(path, ec)) {
            if (ec) return {};
            continue; // Another process created it; inspect a sibling instead.
        }
        std::ofstream file(path / "store-version.txt", std::ios::out | std::ios::trunc);
        file << expected << '\n';
        file.close();
        if (!file) return {}; // Leave any partial new directory intact.
        return path;
    }
    return {};
}
}
