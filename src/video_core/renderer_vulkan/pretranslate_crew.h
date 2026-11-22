// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "common/types.h"
#include "video_core/renderer_vulkan/compiler_worker_budget.h"

namespace Vulkan {

/// FIX-SGT-099: the manifest pre-translation's workers. They are as many as the logical CPUs while
/// the splash precompile holds the game, and a few once it doesn't, also when the hold ends before
/// they are done. One way: a crew in play never widens again.
class PretranslateCrew {
public:
    static constexpr u32 SplashWidth(u32 logical) {
        return CompilerWorkerBudget::StartupPipelineWorkers(logical);
    }
    static constexpr u32 PlayWidth(u32 logical) {
        return std::clamp(logical / 4, 1u, 4u);
    }

    /// `held`: the game is held now. `follows_hold` false: the crew stays as it started.
    PretranslateCrew(u32 logical, bool held, bool follows_hold, size_t records_)
        : threads{held ? SplashWidth(logical) : PlayWidth(logical)}, play{PlayWidth(logical)},
          follows{held && follows_hold}, records{records_}, in_play{!held}, working{threads} {}

    u32 Threads() const {
        return threads;
    }

    /// One on each of the Threads() threads, for as long as it works.
    class Worker {
    public:
        /// Runs the policy of the moment on the calling thread. Under the crew's mutex, so a
        /// release can't come between the choice and the policy, which would undo the release.
        template <typename Splash, typename Play>
        Worker(PretranslateCrew& crew_, Splash&& splash_policy, Play&& play_policy) : crew{crew_} {
            std::scoped_lock lk{crew.policy_mutex};
            if (crew.in_play.load()) {
                std::forward<Play>(play_policy)();
            } else {
                std::forward<Splash>(splash_policy)();
            }
        }
        ~Worker() {
            if (counted) --crew.working;
        }
        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;

        /// The next record, or none when the worker is to end. Whether it stays is decided before
        /// it takes one: a record taken by a worker that leaves would never be translated.
        std::optional<size_t> TakeRecord() {
            if (crew.in_play.load()) {
                // In play no record starts while more than the play width are still working.
                for (u32 now = crew.working.load(); now > crew.play;) {
                    if (crew.working.compare_exchange_weak(now, now - 1)) {
                        counted = false;
                        return std::nullopt;
                    }
                }
            }
            const size_t index = crew.next++;
            return index < crew.records ? std::optional{index} : std::nullopt;
        }

    private:
        PretranslateCrew& crew;
        bool counted = true;
    };

    /// For the thread that joins them, before it does: returns when the workers have all ended, or
    /// when `holds` turned false and `to_play` lowered them. It is given the play width and lowers
    /// them at once, in the middle of a record too (a translation can take seconds).
    template <typename Holds, typename ToPlay>
    void Watch(Holds&& holds, ToPlay&& to_play) {
        if (!follows) return;
        while (working.load() != 0) {
            if (!holds()) {
                std::scoped_lock lk{policy_mutex};
                in_play = true;
                to_play(std::min(threads, play));
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }

private:
    const u32 threads;
    const u32 play;
    const bool follows;
    const size_t records;
    std::mutex policy_mutex;
    std::atomic<bool> in_play;
    std::atomic<u32> working;
    std::atomic<size_t> next{0};
};

} // namespace Vulkan
