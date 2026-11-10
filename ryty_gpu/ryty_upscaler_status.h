// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ryGT FSR-204: the upscaler in use, shown in the game window's title ("GTSport DLSS FG Quality";
// FIX-GTS-439: nothing when no upscaler is active). The presenter reports it every frame; the
// window thread, woken by an SDL event, sets the title only when it changed.

#pragma once

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

namespace Vulkan::RytyUpscalerStatus {

inline std::mutex g_mutex;
inline std::string g_status;
inline bool g_changed{};
// Observed rendered scene extent, not the full-resolution composition attachment.
inline std::atomic<std::uint64_t> g_scene_extent{};
inline void ObserveScene(unsigned width, unsigned height) {
    g_scene_extent.store((std::uint64_t(width) << 32) | height, std::memory_order_relaxed);
}


/// Presenter (any thread): the upscaler used for this frame.
inline void Report(std::string status) {
    {
        std::scoped_lock lock{g_mutex};
        if (status == g_status) {
            return;
        }
        g_status = std::move(status);
        g_changed = true;
    }
    SDL_Event wake{};
    wake.type = SDL_EVENT_USER;
    SDL_PushEvent(&wake);
}

/// FIX-GTS-439: the upscaler's quality mode from the output/scene scale (the usual FSR, DLSS and
/// XeSS ratios: 1.3 Ultra Quality, 1.5 Quality, 1.7 Balanced, 2.0 Performance, 3.0 Ultra
/// Performance; 1.0 Native AA). Any other scale is shown as itself, e.g. "1.6x".
inline std::string QualityMode(unsigned output_width, unsigned output_height, unsigned scene_width,
                               unsigned scene_height) {
    if (scene_width == 0 || scene_height == 0 || output_width == 0 || output_height == 0) return "";
    const double scale = std::max(double(output_width) / scene_width, double(output_height) / scene_height);
    struct Mode {
        double scale;
        const char* name;
    };
    static constexpr Mode modes[] = {{1.0, "Native AA"},   {1.3, "Ultra Quality"}, {1.5, "Quality"},
                                     {1.7, "Balanced"},    {2.0, "Performance"},   {3.0, "Ultra Performance"}};
    for (const auto& mode : modes) {
        if (std::abs(scale - mode.scale) <= 0.06) return mode.name;
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%.1fx", scale);
    return text;
}

inline void ReportResolution(unsigned output_width, unsigned output_height,
                             unsigned fallback_width, unsigned fallback_height,
                             std::string upscaler, bool generating_frames = false) {
    const auto scene = g_scene_extent.load(std::memory_order_relaxed);
    const unsigned width = scene ? unsigned(scene >> 32) : fallback_width;
    const unsigned height = scene ? unsigned(scene) : fallback_height;
    if (const auto suffix = upscaler.find(" temporal"); suffix != std::string::npos)
        upscaler.erase(suffix);
    // Major version only: FSR 4.1.2 shows as FSR 4; XeSS has no number.
    if (const auto dot = upscaler.find('.'); dot != std::string::npos) {
        const auto end = upscaler.find(' ', dot);
        upscaler.erase(dot, end == std::string::npos ? end : end - dot);
    }
    if (upscaler.starts_with("XeSS")) upscaler = "XeSS";
    // FIX-GTS-439: the title shows the upscaler only when one is active; without one (and without
    // frame generation) it is just the window title. An empty status tells the window thread so.
    // The status is short: "<upscaler>[ FG] <quality mode>", e.g. "DLSS Quality", "FSR 4 FG
    // Performance"; frame generation alone is "FG".
    if (upscaler.empty() || upscaler == "no upscaling" || upscaler == "high-resolution UI") {
        Report(generating_frames ? std::string{"FG"} : std::string{});
        return;
    }
    if (generating_frames) upscaler += " FG";
    Report(upscaler + " " + QualityMode(output_width, output_height, width, height));
}

/// Window thread: the new status once, after it changed.
inline std::optional<std::string> Take() {
    std::scoped_lock lock{g_mutex};
    if (!g_changed) {
        return std::nullopt;
    }
    g_changed = false;
    return g_status;
}

} // namespace Vulkan::RytyUpscalerStatus
