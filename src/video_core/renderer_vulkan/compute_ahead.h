// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// FEAT-SGT-118: the compute recipes the splash builds ahead, and the gate a build waits at. Both
// stop at the game's release: a driver compile in play costs frames. Without Vulkan, for its tests.

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <unordered_set>
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
        std::unordered_set<u64> attempted; ///< the recipes built or refused by the driver
    };

    /// From the pre-translation's workers. False when not kept: a copy, or the game plays. `hash`,
    /// when the game is held: the recipe's, whether kept now or already.
    template <typename Held>
    bool Add(std::vector<u32>&& words, Held&& held, u64* hash = nullptr) {
        if (!held()) {
            return false;
        }
        const u64 key = XXH3_64bits(words.data(), words.size() * sizeof(u32));
        if (hash) {
            *hash = key;
        }
        std::scoped_lock lk{mutex};
        return recipes.try_emplace(key, std::move(words)).second;
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
                counts.attempted.insert(hash);
            } else if (!held()) {
                ++counts.left; // refused at the gate: the game was released while it waited
            } else {
                ++counts.failed;
                counts.attempted.insert(hash);
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

/// The pre-translation maps each record to its permutation in recipe-perms.txt, and a later start
/// skips the records it maps (FIX-SGT-119). A compute record is mapped only once its recipe was
/// built or refused here: a start that ended, or let the game go, before then leaves it unmapped,
/// so the next start replays it and builds it ahead. The other records are mapped at once, as
/// before: a start stopped during these builds keeps them.
class ComputeAheadRecords {
public:
    using Line = std::array<u64, 3>; ///< program, the record's SPIR-V hash, permutation

    /// A compute record and its recipe; none when it wasn't kept (the game played).
    void Note(u64 program, u64 record_spirv, std::optional<u64> recipe) {
        std::scoped_lock lk{mutex};
        records[{program, record_spirv}] = recipe;
    }

    /// Moves the noted records' lines out of `lines`, to be kept for after the builds.
    std::vector<Line> TakeLines(std::vector<Line>& lines) const {
        std::scoped_lock lk{mutex};
        std::vector<Line> taken;
        std::erase_if(lines, [&](const Line& line) {
            if (!records.contains({line[0], line[1]})) {
                return false;
            }
            taken.push_back(line);
            return true;
        });
        return taken;
    }

    /// Removes from `lines` those whose recipe `attempted` doesn't hold; how many.
    size_t DropUnbuilt(std::vector<Line>& lines, const std::unordered_set<u64>& attempted) const {
        std::scoped_lock lk{mutex};
        return std::erase_if(lines, [&](const Line& line) {
            const auto it = records.find({line[0], line[1]});
            return it != records.end() && (!it->second || !attempted.contains(*it->second));
        });
    }

private:
    mutable std::mutex mutex;
    std::map<std::pair<u64, u64>, std::optional<u64>> records;
};

} // namespace Vulkan
