// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>

namespace Native {
// Session starts do not depend on scripts being reloaded. A retiring session
// must not re-enable CPU snapshots over a newer session's GPU-owned buffers.
class MenuSnapshotSession {
public:
    void Begin() {
        auto token = state.load(std::memory_order_relaxed);
        while (!state.compare_exchange_weak(token, (token + 2) | 1,
                                            std::memory_order_acq_rel)) {}
    }
    std::uint64_t Ending() const { return state.load(std::memory_order_acquire); }
    void End(std::uint64_t token) {
        const auto idle = token & ~std::uint64_t{1};
        state.compare_exchange_strong(token, idle, std::memory_order_acq_rel);
    }
    bool Idle() const { return (state.load(std::memory_order_acquire) & 1) == 0; }
private:
    std::atomic<std::uint64_t> state{0};
};
}
