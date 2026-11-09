// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <optional>

namespace Vulkan {
// PERF-GT7-026: a fixed 16 ms window bounds optional per-draw read-ahead work.
class ReadAheadBudget {
public:
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> Deadline(Clock::time_point start, bool waiting) {
        const auto per_call = start + std::chrono::milliseconds{2};
        if (waiting) return per_call;
        if (start - window > std::chrono::milliseconds{16}) {
            window = start;
            used = {};
        }
        const auto left = Clock::duration{std::chrono::milliseconds{3}} - used;
        if (left <= Clock::duration::zero()) return std::nullopt;
        return std::min(per_call, start + left);
    }
    void Charge(Clock::time_point start, Clock::time_point end, bool waiting) {
        if (!waiting && end >= start) used += end - start;
    }
private:
    Clock::time_point window{};
    Clock::duration used{};
};
} // namespace Vulkan
