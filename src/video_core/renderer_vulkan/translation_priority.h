// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>
namespace Vulkan {
inline std::chrono::milliseconds ParseTranslationMaxAge(const char* value) {
    constexpr auto fallback = std::chrono::milliseconds{33};
    if (!value || !*value) return fallback;
    const std::string_view text{value};
    std::int64_t number{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (error != std::errc{} || end != text.data() + text.size() || number < 0 ||
        number > std::numeric_limits<std::int32_t>::max()) return fallback;
    return std::chrono::milliseconds{number};
}
// Caller holds the queue mutex. Already-running/completed jobs are absent.
// Rotate only the prefix so speculative jobs retain their relative order.
template<class Queue, class Job> bool PrioritizeTranslation(Queue& queue, const Job& job) {
    const auto found = std::find(queue.begin(), queue.end(), job);
    if (found == queue.end()) return false;
    std::rotate(queue.begin(), found, std::next(found));
    return true;
}

template<class Queue, class Job> bool RequireTranslation(Queue& queue, const Job& job) {
    if (!PrioritizeTranslation(queue, job)) return false;
    job->required = true;
    return true;
}

// All queue operations below require the same queue mutex. Required jobs keep priority;
// with age control disabled, retain the existing FIFO behavior for optional jobs.
template<class Queue, class Job>
void EnqueueTranslation(Queue& queue, Job job, bool age_control) {
    if (!age_control) {
        queue.push_back(std::move(job));
        return;
    }
    const auto optional = std::find_if(queue.begin(), queue.end(), [](const auto& item) {
        return !item->required;
    });
    queue.insert(optional, std::move(job));
}

template<class Queue, class Time, class Duration>
auto TakeTranslation(Queue& queue, Time now, Duration max_age) {
    auto job = std::move(queue.front());
    queue.pop_front();
    const bool expired = max_age > Duration::zero() && !job->required &&
                         now - job->queued_at > max_age;
    if (expired) {
        {
            std::scoped_lock lock{job->mutex};
            // No worker claimed this job, and readers inspect these only after done.
            job->prepared_translation.reset();
            job->info.reset();
            job->cancelled = true;
            job->done = true;
        }
        job->cv.notify_all();
    }
    return std::pair{std::move(job), expired};
}
}
