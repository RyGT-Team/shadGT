// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace VideoCore {

// FEAT-SGT-002: RyGT-GPU-Capture's hotkeys, built into the renderer (no Python, any renderer
// build). Shift+F8: a burst of consecutive frames as PNGs, for flicker and transitions.
// Ctrl+F8: starts or stops a performance recording (process CPU and memory every 250 ms, frames
// presented, nvidia-smi once a second when the driver has it). Plain F8 stays the frame dump of
// builds with diagnostics. Output goes to captures\ next to the game executable.
void CaptureBurst();
void TogglePerformanceRecording();
void StopPerformanceRecording(); // at shutdown

} // namespace VideoCore
