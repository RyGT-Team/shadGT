// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/translation_priority.h"

using namespace std::chrono_literals;
namespace {
struct Job {
    explicit Job(int id_) : id{id_} {}
    int id;
    bool required{}, cancelled{};
    std::atomic<bool> done{};
    std::chrono::steady_clock::time_point queued_at{};
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<void> prepared_translation;
    std::unique_ptr<int> info;
};
using Jobs = std::deque<std::shared_ptr<Job>>;
}

TEST(TranslationAge, EnvironmentParserRejectsInvalidAndOverflowValues) {
    using Vulkan::ParseTranslationMaxAge;
    EXPECT_EQ(ParseTranslationMaxAge(nullptr), 33ms);
    EXPECT_EQ(ParseTranslationMaxAge(""), 33ms);
    EXPECT_EQ(ParseTranslationMaxAge("0"), 0ms);
    EXPECT_EQ(ParseTranslationMaxAge("27"), 27ms);
    EXPECT_EQ(ParseTranslationMaxAge("-1"), 33ms);
    EXPECT_EQ(ParseTranslationMaxAge("12ms"), 33ms);
    EXPECT_EQ(ParseTranslationMaxAge("2147483648"), 33ms);
    EXPECT_EQ(ParseTranslationMaxAge("999999999999999999999999"), 33ms);
}

TEST(TranslationAge, NewestOptionalFollowsRequiredAndDisabledModeIsFifo) {
    Jobs jobs;
    auto old = std::make_shared<Job>(1), required = std::make_shared<Job>(2), recent = std::make_shared<Job>(3);
    required->required = true;
    Vulkan::EnqueueTranslation(jobs, old, true);
    Vulkan::EnqueueTranslation(jobs, required, true);
    Vulkan::EnqueueTranslation(jobs, recent, true);
    ASSERT_EQ(jobs.size(), 3u);
    EXPECT_EQ(jobs[0], required);
    EXPECT_EQ(jobs[1], recent);
    EXPECT_EQ(jobs[2], old);
    jobs.clear();
    Vulkan::EnqueueTranslation(jobs, old, false);
    Vulkan::EnqueueTranslation(jobs, recent, false);
    EXPECT_EQ(jobs.front(), old);
}

TEST(TranslationAge, RequiredPromotionCannotExpireAndStaleJobNotifiesWaiter) {
    Jobs jobs;
    auto required = std::make_shared<Job>(1), stale = std::make_shared<Job>(2);
    const auto now = std::chrono::steady_clock::now();
    required->queued_at = stale->queued_at = now - 1s;
    stale->prepared_translation = std::make_shared<int>(42);
    stale->info = std::make_unique<int>(7);
    Vulkan::EnqueueTranslation(jobs, required, true);
    Vulkan::EnqueueTranslation(jobs, stale, true);
    ASSERT_TRUE(Vulkan::RequireTranslation(jobs, required));
    auto [first, expired] = Vulkan::TakeTranslation(jobs, now, 33ms);
    EXPECT_EQ(first, required);
    EXPECT_FALSE(expired);
    EXPECT_FALSE(first->done);
    auto waiter = std::async(std::launch::async, [&] {
        std::unique_lock lock{stale->mutex};
        return stale->cv.wait_for(lock, 2s, [&] { return stale->done.load(); });
    });
    auto [second, was_expired] = Vulkan::TakeTranslation(jobs, now, 33ms);
    EXPECT_EQ(second, stale);
    EXPECT_TRUE(was_expired);
    EXPECT_TRUE(waiter.get());
    EXPECT_TRUE(stale->cancelled);
    EXPECT_FALSE(stale->prepared_translation);
    EXPECT_FALSE(stale->info);
}

TEST(TranslationAge, PromotionAndExpirySerializeOnQueueMutex) {
    for (int i = 0; i < 200; ++i) {
        Jobs jobs;
        std::mutex queue_mutex;
        auto job = std::make_shared<Job>(i);
        const auto now = std::chrono::steady_clock::now();
        job->queued_at = now - 1s;
        jobs.push_back(job);
        bool promoted = false, expired = false;
        std::thread draw([&] {
            std::scoped_lock lock{queue_mutex};
            promoted = Vulkan::RequireTranslation(jobs, job);
        });
        std::thread worker([&] {
            std::scoped_lock lock{queue_mutex};
            auto result = Vulkan::TakeTranslation(jobs, now, 33ms);
            expired = result.second;
        });
        draw.join();
        worker.join();
        EXPECT_NE(promoted, expired);
        EXPECT_EQ(job->cancelled, expired);
        EXPECT_EQ(job->done.load(), expired);
    }
}
