// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace Native {

class PendingImageWork {
public:
    PendingImageWork() {
        const char* value = std::getenv("SHADGT_READ_EMPTY_IMAGE_QUEUES"); // PERF-SGT-223
        enabled = value && value[0] == '1' && value[1] == '\0';
    }

    void Publish(bool any) {
        if (enabled) {
            pending.store(any, std::memory_order_release);
        }
    }

    template <class T>
    bool Any(std::mutex& mutex, const std::vector<T>& queue) const {
        if (enabled && !pending.load(std::memory_order_acquire)) {
            return false;
        }
        std::scoped_lock lock{mutex};
        return !queue.empty();
    }

private:
    bool enabled = false;
    std::atomic<bool> pending{false};
};

} // namespace Native
