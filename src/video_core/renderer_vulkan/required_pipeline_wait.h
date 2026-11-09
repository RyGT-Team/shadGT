// Copyright 2026 RyGT team. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <mutex>
#include <utility>

namespace Vulkan {

// The counter and replacement check are accessed only while holding the lookup lock.
inline bool CanReplaceShader(unsigned int waiters, bool builds_busy) {
    return waiters == 0 && !builds_busy;
}

inline bool CanUnlockRequired(bool speculative, bool legacy, bool fallback, bool deferred) {
    return !speculative && !legacy && !fallback && !deferred;
}

inline bool CanCancelRequiredBuild(unsigned int waiters) {
    return waiters == 0;
}

template <typename Mutex, typename Wait>
void WaitOutsideLookup(std::unique_lock<Mutex>& lock, unsigned int& waiters, Wait&& wait) {
    ++waiters;
    lock.unlock();
    try {
        std::forward<Wait>(wait)();
    } catch (...) {
        lock.lock();
        --waiters;
        throw;
    }
    lock.lock();
    --waiters;
}

} // namespace Vulkan
