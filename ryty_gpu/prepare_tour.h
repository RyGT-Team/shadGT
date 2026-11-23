// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

// FEAT-SGT-127: when one slice of the preparation tour is over, and whether it ran whole. The menu
// script queues a slice's loads at once; the scene then runs them one after the other, and the load
// hooks see each one start. Nothing tells when the last one is done, so a slice ends when no load
// has started for a while, derived from the slice's own gaps. Pure logic on the clock the caller
// passes (seconds), so it can be tested.
namespace PrepareTour {

struct Slice {
    std::uint32_t loads = 0;
    double last = 0.0;        // when the latest load started
    double longest_gap = 0.0; // between two loads of this slice
    bool open = false;
};

/// How long without a load ends a slice: six times its longest gap between two loads, 20 s at least
/// and 120 s at most; 30 s while it has had a single load. Measured on one PC (Ryzen 7 8700G, RTX
/// 4070): a car with its interior every 1.0 s (1.4 s at most), a course layout every 5.5 s (8.3 s at
/// most); a PC twice as slow still ends a course slice after about 100 s.
inline double QuietSeconds(const Slice& slice) {
    if (slice.loads < 2) {
        return 30.0;
    }
    return std::clamp(6.0 * slice.longest_gap, 20.0, 120.0);
}

/// A load starts at `now`. Returns whether it opens the slice.
inline bool NoteLoad(Slice& slice, double now) {
    const bool opens = !slice.open;
    if (opens) {
        slice = Slice{};
        slice.open = true;
    } else {
        slice.longest_gap = std::max(slice.longest_gap, now - slice.last);
    }
    slice.last = now;
    ++slice.loads;
    return opens;
}

/// Whether an open slice has had no load for its quiet time at `now`.
inline bool WentQuiet(const Slice& slice, double now) {
    return slice.open && now - slice.last >= QuietSeconds(slice);
}

/// Whether a slice that saw `loads` loads ran whole, `queued` being what the menu script queued for
/// it. Unknown (nothing read) or nothing queued is not whole.
inline bool RanWhole(std::uint32_t loads, std::optional<std::uint32_t> queued) {
    return queued.has_value() && *queued > 0 && loads >= *queued;
}

/// FEAT-SGT-129: what the tour loads besides the cars and courses. SHADGT_PREPARE_LEAVE_OUT lists, with
/// commas, what it leaves out: drivers, crowds, wheels (the spare rims), reward (the reward screen).
struct Choices {
    bool drivers = true;
    bool crowds = true;
    bool wheels = true;
    bool reward = true;
    unsigned unknown = 0; // words that name none of them
};

inline Choices ParseLeaveOut(std::string_view text) {
    Choices choices;
    while (!text.empty()) {
        const size_t comma = text.find(',');
        std::string_view word = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        while (!word.empty() && (word.front() == ' ' || word.front() == '\t')) {
            word.remove_prefix(1);
        }
        while (!word.empty() && (word.back() == ' ' || word.back() == '\t')) {
            word.remove_suffix(1);
        }
        if (word.empty()) {
            continue;
        }
        if (word == "drivers") {
            choices.drivers = false;
        } else if (word == "crowds") {
            choices.crowds = false;
        } else if (word == "wheels") {
            choices.wheels = false;
        } else if (word == "reward") {
            choices.reward = false;
        } else {
            ++choices.unknown;
        }
    }
    return choices;
}

/// FEAT-SGT-129: how a part ends. One that ran whole is done. One cut short by the next part's car
/// change runs again. One that stopped short by itself (a load that never finished, or never started)
/// runs again once; stopped short a second time, it is left as it is, so the parts after it still run.
enum class End { Done, Again, GiveUp };

inline End HowItEnds(bool whole, bool cut, bool stopped_before) {
    if (whole) {
        return End::Done;
    }
    return cut || !stopped_before ? End::Again : End::GiveUp;
}

} // namespace PrepareTour
