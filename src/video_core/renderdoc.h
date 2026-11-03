// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include "common/types.h"

namespace VideoCore {

#ifndef NDEBUG
/// Loads renderdoc dynamic library module.
void LoadRenderDoc();

/// Begins a capture if a renderdoc instance is attached.
void StartCapture();

/// Ends current renderdoc capture.
void EndCapture();

/// Triggers capturing process.
void TriggerCapture();

/// Sets output directory for captures
void SetOutputDir(const std::filesystem::path& path, const std::string& prefix);

/// Returns true when RenderDoc API was loaded and is usable.
bool IsRenderDocLoaded();

#else
inline void LoadRenderDoc() noexcept {}
inline void StartCapture() noexcept {}
inline void EndCapture() noexcept {}
inline void TriggerCapture() noexcept {}
inline void SetOutputDir(const std::filesystem::path&, const std::string&) noexcept {}
inline constexpr bool IsRenderDocLoaded() noexcept { return false; }
#endif

enum class ScreenshotRequest : u32 {
    None = 0,
    GameOnly = 1,
    WithOverlays = 2,
};

struct ScreenshotRequests {
    u32 game_only_count{};
    u32 with_overlays_count{};
};

/// Queues an in-emulator screenshot request to be consumed by the presenter.
void RequestScreenshot(ScreenshotRequest request);

/// Queues a "game only" screenshot written to the given path (used by IPC SCREENSHOT).
void RequestScreenshotToPath(const std::filesystem::path& path);

/// Takes up to max_count paths queued by RequestScreenshotToPath, oldest first.
std::vector<std::filesystem::path> TakeRequestedScreenshotPaths(u32 max_count);

/// Atomically consumes and returns pending "game only" screenshot request counter.
u32 ConsumeGameOnlyScreenshotRequests();

/// Atomically consumes and returns pending "with overlays" screenshot request counter.
u32 ConsumeWithOverlaysScreenshotRequests();

/// Atomically consumes and returns pending screenshot request counters.
ScreenshotRequests ConsumeScreenshotRequests();

} // namespace VideoCore
