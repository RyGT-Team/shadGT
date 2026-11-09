// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

#include "common/types.h"

namespace Libraries::VideoOut {

constexpr bool FastFlipsEnabled(const char* env, std::optional<bool> config,
                                bool native_renderer, bool frame_generation) {
    if (env && *env) return env[0] == '1';
    if (config) return *config;
    return native_renderer && !frame_generation;
}

constexpr std::chrono::nanoseconds FastFlipInterval(std::chrono::nanoseconds vblank, int rate) {
    return std::max(std::chrono::nanoseconds::zero(),
                    vblank * (rate + 1) - std::chrono::milliseconds{1});
}

/// PERF-075: the flip rate the presenter honours with a frame-rate lock. A flip rate of N lets a
/// flip happen only every N+1 vblanks, so a 30 FPS lock on a 60 Hz vblank is rate 1. The lock
/// only ever slows flips down: a game asking for a lower rate keeps it. 0 means no lock.
constexpr int LockedFlipRate(int game_rate, u32 lock_fps, u32 vblank_hz) {
    if (lock_fps == 0 || vblank_hz == 0 || lock_fps >= vblank_hz) {
        return game_rate;
    }
    const int lock_rate = static_cast<int>((vblank_hz + lock_fps - 1) / lock_fps) - 1;
    return std::max(game_rate, lock_rate);
}

} // namespace Libraries::VideoOut
