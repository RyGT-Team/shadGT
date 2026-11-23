// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// FEAT-SGT-118: the compute recipes the splash builds ahead, and the gate a build waits at. Both
// stop at the game's release: a driver compile in play costs frames. Without Vulkan, for its tests.

#include <atomic>
#include <mutex>
#include <stop_token>
#include <unordered_map>
#include <utility>
#include <vector>
#include <xxhash.h>

#include "common/types.h"

namespace Vulkan {

/// Takes the heavy pipeline gate (HeavyPipelineGate) for a build that is wanted only while
/// `wanted()` is true. A full gate is tried again after `wait()` (FIX-SGT-114: no timed waits).
/// `stop` and `wanted()` are asked while waiting and once the gate is taken, so a build that waited
/// across the game's release never reaches the driver. True: the caller holds the gate and builds.
template <typename Gate, typename Wanted, typename Wait>
bool TakeHeavyGate(Gate& gate, std::stop_token stop, Wanted&& wanted, Wait&& wait) {
    while (!gate.try_acquire()) {
        if (stop.stop_requested() || !wanted()) {
            return false;
        }
        wait();
    }
    if (stop.stop_requested() || !wanted()) {
        gate.release();
        return false;
    }
    return true;
}

/// One recipe per distinct code and layout (the game often asks for the same code under another
/// permutation), kept only while the game is held: none is copied or kept once it plays.
class ComputeAheadQueue {
public:
    struct Counts {
        u32 built{};
        u32 failed{};
        u32 left{}; ///< not built: the game was released
    };

    /// From the pre-translation's workers. False when not kept: a copy, or the game plays.
    template <typename Held>
    bool Add(std::vector<u32>&& words, Held&& held) {
        if (!held()) {
            return false;
        }
        const u64 hash = XXH3_64bits(words.data(), words.size() * sizeof(u32));
        std::scoped_lock lk{mutex};
        return recipes.try_emplace(hash, std::move(words)).second;
    }

    size_t Size() const {
        std::scoped_lock lk{mutex};
        return recipes.size();
    }

    /// Builds each recipe once while `held()`, in no order; from the game's release the others are
    /// left. `done` counts each recipe built, failed or left; the queue is empty afterwards.
    template <typename Held, typename Build>
    Counts Run(Held&& held, Build&& build, std::atomic<u64>& done) {
        std::unordered_map<u64, std::vector<u32>> taken;
        {
            std::scoped_lock lk{mutex};
            taken.swap(recipes);
        }
        Counts counts;
        for (auto& [hash, words] : taken) {
            if (!held()) {
                ++counts.left;
            } else if (build(std::as_const(words))) {
                ++counts.built;
            } else {
                ++counts.failed;
            }
            std::vector<u32>{}.swap(words); // built or not, no longer needed
            ++done;
        }
        return counts;
    }

private:
    mutable std::mutex mutex;
    std::unordered_map<u64, std::vector<u32>> recipes;
};

} // namespace Vulkan
