// Diagnostic-only, one-shot race-to-menu arm. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
class A82RaceGate {
public:
    enum : std::uint8_t { Waiting, SawRace, Armed };
    std::uint8_t Observe(bool race) noexcept {
        auto old=state.load(std::memory_order_relaxed);
        for (;;) {
            const auto next = old==Waiting && race ? SawRace : old==SawRace && !race ? Armed : old;
            if (next==old || state.compare_exchange_weak(old,next,std::memory_order_relaxed)) return next;
        }
    }
    std::uint8_t State() const noexcept {return state.load(std::memory_order_relaxed);}
private:
    std::atomic<std::uint8_t> state{Waiting};
};
