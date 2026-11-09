// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ryGT FSR-204: the upscaler in use, shown in the game window's title ("ryGT - Gran Turismo Sport |
// FSR 3.1.4 temporal 1920x1080 -> 2560x1440"). The presenter reports it every frame; the window
// thread, woken by an SDL event, sets the title only when it changed.

#pragma once

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
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

inline void ReportResolution(unsigned output_width, unsigned output_height,
                             unsigned fallback_width, unsigned fallback_height,
                             std::string upscaler, bool generating_frames = false) {
    const auto scene = g_scene_extent.load(std::memory_order_relaxed);
    const unsigned width = scene ? unsigned(scene >> 32) : fallback_width;
    const unsigned height = scene ? unsigned(scene) : fallback_height;
    // FSR-205: "<upscaler> {window}, <output> From <scene>"; the window thread fills in {window}.
    if (const auto suffix = upscaler.find(" temporal"); suffix != std::string::npos)
        upscaler.erase(suffix);
    // Major version only: FSR 4.1.2 shows as FSR 4; XeSS has no number.
    if (const auto dot = upscaler.find('.'); dot != std::string::npos) {
        const auto end = upscaler.find(' ', dot);
        upscaler.erase(dot, end == std::string::npos ? end : end - dot);
    }
    if (upscaler.starts_with("XeSS")) upscaler = "XeSS";
    if (upscaler.empty() || upscaler == "no upscaling" || upscaler == "high-resolution UI")
        upscaler = "No upscaling";
    if (generating_frames) upscaler += " FG";
    std::string status = upscaler + " {window}, " + std::to_string(output_width) + "x" +
                         std::to_string(output_height) + " From " + std::to_string(width) + "x" +
                         std::to_string(height);
    Report(std::move(status));
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
