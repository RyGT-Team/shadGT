#pragma once

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/ryty_config.h"

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
    // GAME-005: the game's own settings file (Common::RytyGameConfigPathW); FIX-230: Unicode
    // and long-path safe.
    const std::string text = Common::RytyReadGameConfig();
    if (text.empty()) {
        return std::nullopt;
    }
    return ParseConfigBool(text, key);
#else
    return std::nullopt;
#endif
}

// Whether `key` is true in user\custom_configs\<serial>.json (the launcher's options).
inline bool ConfigTrue(std::string_view key) { return ConfigBool(key).value_or(false); }

// The number after `key` in the config (the launcher's options), or `fallback`.
inline double ConfigNumber(std::string_view key, double fallback) {
#ifdef _WIN32
    // GAME-005: the game's own settings file (Common::RytyGameConfigPathW).
    const std::string text = Common::RytyReadGameConfig();
    if (text.empty()) {
        return fallback;
    }
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

// NEO-GTS-505: the PS4 Pro mode has one switch for both games, GT7's: SHADGT_NEO=1 (ryGT7's
// runtime reads it as RYGT_NEO), with SHADGT_NEO_TV=1080 for a Pro on a 1080p TV (2160, the
// default, for a 4K TV). GT Sport's runtime does not know it, so for GT Sport the renderer answers
// the game itself. SHADGT_PS4_PRO and the GPU.ps4_pro config of RyGTS's launcher are still read
// after it: SHADGT_PS4_PRO overrides the config (0 forces base mode); the config option means
// Neo 1080.
inline ProMode GetProMode() {
    static const ProMode mode = [] {
        if (const char* neo = std::getenv("SHADGT_NEO"); neo && std::string_view{neo} == "1") {
            const char* tv = std::getenv("SHADGT_NEO_TV");
            return tv && std::string_view{tv} == "1080" ? ProMode::Neo1080 : ProMode::Neo4K;
        }
        const char* value = std::getenv("SHADGT_PS4_PRO");
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
