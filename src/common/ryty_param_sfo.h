// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "common/game_title.h"

namespace Common {

inline bool RytyMountedMetadataEnabled() {
    const char* setting = std::getenv("SHADGT_HDR_MOUNTED_METADATA");
    return !setting || std::string_view(setting) != "0";
}

// FIX-SGT-058: resolve the metadata as Ryty mounts /app0. The startup reader also runs
// in DllMain, before the guest filesystem API can be called. Ryty's native resolver
// returns a libstdc++ filesystem::path, which cannot cross into the renderer's MSVC STL.
inline std::filesystem::path RytyParamSfoPath(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    if (root.empty()) return {};
    const fs::path relative = fs::path("sce_sys") / "param.sfo";
    const fs::path top = App0Folder(root) / relative;
    std::error_code ec;
    if (fs::exists(top, ec)) return top;
    if (!RytyMountedMetadataEnabled()) return top;

    const auto app0 = fs::weakly_canonical(root / "app0", ec);
    const auto find = [&](const fs::path& layer) -> fs::path {
        if (layer.empty()) return {};
        const auto folder = fs::weakly_canonical(root / layer, ec);
        if (ec || folder == app0 || folder.parent_path() == app0) return {};
        const auto metadata = folder / relative;
        return fs::exists(metadata, ec) ? metadata : fs::path{};
    };

    // An explicit layer list replaces the INI paths, including when its folders are absent.
#ifdef _WIN32
    const auto* layers = _wgetenv(L"RYGT_APP0_LAYERS");
#else
    const auto* layers = std::getenv("RYGT_APP0_LAYERS");
#endif
    if (layers && *layers) {
        std::basic_string_view list(layers);
        while (!list.empty()) {
            const auto end = list.find(';');
            if (const auto path = find(fs::path(list.substr(0, end))); !path.empty()) return path;
            if (end == list.npos) break;
            list.remove_prefix(end + 1);
        }
        return top;
    }

    const auto trim = [](std::string_view value) {
        const auto first = value.find_first_not_of(" \t\r");
        if (first == value.npos) return std::string_view{};
        return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
    };
    std::ifstream ini(RygtIniPath(root), std::ios::binary);
    fs::path game, update;
    for (std::string line; std::getline(ini, line);) {
        std::string_view text(line);
        if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
        const auto equals = text.find('=');
        if (equals == text.npos) continue;
        const auto key = trim(text.substr(0, equals));
        const auto value = trim(text.substr(equals + 1));
        if (value.empty() || (key != "game" && key != "update")) continue;
        (key == "update" ? update : game) = std::u8string(value.begin(), value.end());
    }
    if (const auto path = find(update); !path.empty()) return path;
    if (const auto path = find(game); !path.empty()) return path;
    return top;
}

inline std::filesystem::path RytyParamSfoPath() {
    std::error_code ec;
    return RytyParamSfoPath(std::filesystem::current_path(ec));
}

} // namespace Common
