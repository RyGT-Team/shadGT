#pragma once

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/game_title.h"
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

// GT Sport's PS4 Pro mode, which the renderer answers to the game (GT Sport's runtime has none;
// GT7's is its runtime's, read through sceKernelIsNeoMode, and this stays Base there as before).
// NEO-GTS-505: in order, SHADGT_PS4_PRO (0 forces the base mode, 1080, 1 or 2160), then the switch
// GT7 uses, SHADGT_NEO=1 with SHADGT_NEO_TV=1080 for a Pro on a 1080p TV (2160, the default, for
// a 4K TV), then the GPU.ps4_pro config of RyGTS's launcher (Neo 1080).
inline ProMode GetProMode() {
    static const ProMode mode = [] {
        if (Common::IsGt7()) {
            return ProMode::Base;
        }
        if (const char* value = std::getenv("SHADGT_PS4_PRO")) {
            return ParseProMode(std::string_view{value});
        }
        if (const char* neo = std::getenv("SHADGT_NEO"); neo && std::string_view{neo} == "1") {
            const char* tv = std::getenv("SHADGT_NEO_TV");
            return tv && std::string_view{tv} == "1080" ? ProMode::Neo1080 : ProMode::Neo4K;
        }
        return ProConfigEnabled() ? ProMode::Neo1080 : ProMode::Base;
    }();
    return mode;
}

constexpr bool IsProMode(ProMode mode) {
    return mode != ProMode::Base;
}

} // namespace Ryty
