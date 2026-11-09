// Copyright 2026 RyGT team. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Common {
inline bool ExactNativePriorityOptIn(const char* value) {
    return value && value[0] == '1' && value[1] == '\0';
}
inline bool NativeCpuPriorityRequested() {
    static const bool enabled = [] {
        const char* value = std::getenv("RYGT_NATIVE_GPU_PRIORITY");
        return ExactNativePriorityOptIn(value);
    }();
#ifndef NDEBUG
    static const bool compare = [] {
        const char* value = std::getenv("RYGT_NATIVE_CPU_PRIORITY_AB");
        return ExactNativePriorityOptIn(value);
    }();
    static const auto started = std::chrono::steady_clock::now();
    if (enabled && compare) {
        const auto window = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - started).count() / 10;
        return window % 2 != 0;
    }
#endif
    return enabled;
}

class NativeThreadPriority {
public:
    NativeThreadPriority() = default;
    NativeThreadPriority(const NativeThreadPriority&) = delete;
    NativeThreadPriority& operator=(const NativeThreadPriority&) = delete;
    ~NativeThreadPriority() { Update(false); }

    void Update(bool requested) {
#ifdef _WIN32
        if (!requested && !owned && !externally_changed) return;
        const HANDLE thread = GetCurrentThread();
        const int current = GetThreadPriority(thread);
        if (current == THREAD_PRIORITY_ERROR_RETURN) return;
        if (owned && current != THREAD_PRIORITY_ABOVE_NORMAL) {
            owned = false;
            externally_changed = true;
        }
        if (!requested) {
            if (owned && !SetThreadPriority(thread, original)) return;
            owned = false;
            externally_changed = false;
            return;
        }
        if (!owned && !externally_changed && current < THREAD_PRIORITY_ABOVE_NORMAL) {
            original = current;
            owned = SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL) != 0;
        }
#else
        (void)requested;
#endif
    }

private:
    int original{};
    bool owned{};
    bool externally_changed{};
};
} // namespace Common
