// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
namespace Vulkan {
inline bool DelayFailedTranslationRetry(std::chrono::steady_clock::time_point deadline,
    std::chrono::steady_clock::time_point now, bool speculative, bool same_inputs) {
    return speculative && same_inputs && now < deadline;
}

// FIX-SGT-094: translations refused for lack of SRT walker storage. A shader whose walker did not
// fit is translated again only once a walker that large could fit (memo). While the storage is at
// its wall, a shader not refused yet may still need room nobody has: the command thread spends at
// most WalkerWallBudget::budget per window on translations it then refuses, a time and not a
// count whatever the CPU's speed (refused shaders translated again on each of their draws, each
// try a front end and a scan of every segment, held GT7 at 1 fps after its 510-car tour).
struct WalkerWallBudget {
    static constexpr std::chrono::milliseconds window{200};
    static constexpr std::chrono::milliseconds budget{5};
    std::chrono::steady_clock::time_point window_start{};
    std::chrono::steady_clock::duration spent{};
};
inline void RollWalkerWallWindow(WalkerWallBudget& wall, std::chrono::steady_clock::time_point now) {
    if (now - wall.window_start >= WalkerWallBudget::window) {
        wall.window_start = now;
        wall.spent = {};
    }
}
enum class WalkerGate { Translate, SkipRefused, SkipWall };
inline WalkerGate DecideWalkerGate(bool refused_before, bool may_fit, bool at_wall,
                                   WalkerWallBudget& wall, std::chrono::steady_clock::time_point now) {
    if (refused_before && !may_fit) return WalkerGate::SkipRefused;
    if (!at_wall) return WalkerGate::Translate;
    RollWalkerWallWindow(wall, now);
    return wall.spent < WalkerWallBudget::budget ? WalkerGate::Translate : WalkerGate::SkipWall;
}
/// A translation the storage refused at `now`, started at `start` (when DecideWalkerGate let it).
inline void ChargeWalkerWall(WalkerWallBudget& wall, std::chrono::steady_clock::time_point start,
                             std::chrono::steady_clock::time_point now) {
    RollWalkerWallWindow(wall, now);
    wall.spent += now - start;
}
}
