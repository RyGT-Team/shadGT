#pragma once

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Ryty {

enum class ProMode { Base, Neo1080, Neo4K };

constexpr ProMode ParseProMode(std::string_view value) {
    if (value == "1080") {
        return ProMode::Neo1080;
    }
    if (value == "1" || value == "2160") {
        return ProMode::Neo4K;
    }
    return ProMode::Base;
}

inline std::optional<bool> ParseConfigBool(std::string_view text, std::string_view key) {
    const std::string quoted = "\"" + std::string{key} + "\"";
    const size_t found = text.find(quoted);
    if (found == std::string::npos) return std::nullopt;
    const size_t value = text.find_first_not_of(" \t\r\n:", found + quoted.size());
    if (value == std::string::npos) return std::nullopt;
    if (text.substr(value, 4) == "true") return true;
    if (text.substr(value, 5) == "false") return false;
    return std::nullopt;
}

// The launcher's explicit boolean, if present.
inline std::optional<bool> ConfigBool(std::string_view key) {
#ifdef _WIN32
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::nullopt;
    }
    std::string config{path, length};
    config = config.substr(0, config.find_last_of("\\/") + 1) + "user\\custom_configs\\CUSA03220.json";
    std::FILE* file = std::fopen(config.c_str(), "rb");
    if (!file) {
        return std::nullopt;
    }
    std::string text;
    char chunk[4096];
    while (const size_t n = std::fread(chunk, 1, sizeof(chunk), file)) {
        text.append(chunk, n);
    }
    std::fclose(file);
    return ParseConfigBool(text, key);
#else
    return std::nullopt;
#endif
}

// Whether `key` is true in user\custom_configs\CUSA03220.json (the launcher's options).
inline bool ConfigTrue(std::string_view key) { return ConfigBool(key).value_or(false); }

// The number after `key` in the config (the launcher's options), or `fallback`.
inline double ConfigNumber(std::string_view key, double fallback) {
#ifdef _WIN32
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return fallback;
    }
    std::string config{path, length};
    config = config.substr(0, config.find_last_of("\\/") + 1) + "user\\custom_configs\\CUSA03220.json";
    std::FILE* file = std::fopen(config.c_str(), "rb");
    if (!file) {
        return fallback;
    }
    std::string text;
    char chunk[4096];
    while (const size_t n = std::fread(chunk, 1, sizeof(chunk), file)) {
        text.append(chunk, n);
    }
    std::fclose(file);
    const std::string quoted = "\"" + std::string{key} + "\"";
    const size_t found = text.find(quoted);
    if (found == std::string::npos) {
        return fallback;
    }
    const size_t value = text.find_first_not_of(" \t\r\n:", found + quoted.size());
    if (value == std::string::npos) {
        return fallback;
    }
    char* end = nullptr;
    const double number = std::strtod(text.c_str() + value, &end);
    return end == text.c_str() + value ? fallback : number;
#else
    return fallback;
#endif
}

// GPU.ps4_pro is true in the config (the launcher's PS4 Pro option).
inline bool ProConfigEnabled() {
    return ConfigTrue("ps4_pro");
}

// RYGT_PS4_PRO overrides the config (0 forces base mode); the config option means Neo 1080.
inline ProMode GetProMode() {
    static const ProMode mode = [] {
        const char* value = std::getenv("RYGT_PS4_PRO");
        if (value) {
            return ParseProMode(std::string_view{value});
        }
        return ProConfigEnabled() ? ProMode::Neo1080 : ProMode::Base;
    }();
    return mode;
}

constexpr bool IsProMode(ProMode mode) {
    return mode != ProMode::Base;
}

constexpr std::pair<unsigned, unsigned> ProOutputSize(ProMode mode, unsigned width,
                                                     unsigned height) {
    if (mode == ProMode::Neo4K) {
        return {3840, 2160};
    }
    if (mode == ProMode::Neo1080) {
        return {1920, 1080};
    }
    return {width, height};
}

} // namespace Ryty
