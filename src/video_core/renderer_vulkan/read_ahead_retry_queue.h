// Copyright 2026 RyGT team. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstddef>
#include <deque>
#include <new>
#include <optional>
#include <utility>

namespace Vulkan {
// State owns every byte it will use on retry; callers validate eligibility before Keep.
template <typename State>
class ReadAheadRetryQueue {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t MaxStates = 512;
    static constexpr std::size_t MaxBytes = 8 * 1024 * 1024;

    bool Keep(State state) {
        if (state.bytes > MaxBytes) return false;
        while (!states.empty() &&
               (states.size() >= MaxStates || bytes > MaxBytes - state.bytes)) {
            bytes -= states.front().bytes;
            states.pop_front();
        }
        const auto owned = state.bytes;
        try {
            states.push_back(std::move(state));
            bytes += owned;
            return true;
        } catch (const std::bad_alloc&) {
            return false; // Optional retry may be discarded under memory pressure.
        }
    }
    std::optional<State> TakeReady(Clock::time_point now) {
        for (std::size_t remaining = states.size(); remaining; --remaining) {
            State state = std::move(states.front());
            bytes -= state.bytes;
            states.pop_front();
            if (now - state.first >= std::chrono::seconds{3}) continue;
            if (now < state.next) { Keep(std::move(state)); continue; }
            return state;
        }
        return std::nullopt;
    }
    bool Retry(State state, Clock::time_point now) {
        state.next = now + std::chrono::milliseconds{5};
        return Keep(std::move(state));
    }
    bool empty() const { return states.empty(); }
    std::size_t size() const { return states.size(); }
    std::size_t retained_bytes() const { return bytes; }
    void clear() { states.clear(); bytes = 0; }

private:
    std::deque<State> states;
    std::size_t bytes{};
};
} // namespace Vulkan
