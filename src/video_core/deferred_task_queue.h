// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>
#include <utility>
namespace Storage {
// A single mutex protects both work publication and the wait predicate. A stop
// request wakes the reader, but already-published writes drain before it exits.
template<class Task> class DeferredTaskQueue {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::queue<Task> tasks;
public:
    void Push(Task&& task) {
        {
            std::scoped_lock lock{mutex};
            tasks.push(std::move(task));
        }
        changed.notify_one();
    }
    std::optional<Task> Pop(const std::stop_token& stop) {
        std::unique_lock lock{mutex};
        changed.wait(lock, stop, [&] { return !tasks.empty(); });
        if (tasks.empty()) return std::nullopt;
        Task task=std::move(tasks.front());
        tasks.pop();
        return task;
    }
};
}
