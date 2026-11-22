// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// FIX-SGT-099: the pre-translation's workers fall back to the play width, priority and cores when
// the splash precompile's hold ends.

#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/pretranslate_crew.h"

#ifdef _WIN32
#include <windows.h>
#include "common/thread.h"
#endif

using Vulkan::PretranslateCrew;
using namespace std::chrono_literals;

namespace {

constexpr u32 LogicalCpus[]{1, 2, 4, 6, 8, 12, 16, 20, 24, 32};

// False when it never came, so a broken crew fails its test and doesn't hang it.
bool Await(const std::function<bool()>& ready) {
    const auto until = std::chrono::steady_clock::now() + 20s;
    while (!ready()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// The threads as PretranslateManifest starts, watches and joins them.
struct Harness {
    Harness(u32 logical, size_t records, bool held_ = true, bool follows = true)
        : held{held_}, crew{logical, held_, follows, records}, taken(records) {}

    void Start(std::function<void(size_t)> body) {
        for (u32 t = 0; t < crew.Threads(); ++t) {
            threads.emplace_back([this, body] {
                try {
                    PretranslateCrew::Worker worker{crew, [this] { ++entered_splash; },
                                                    [this] { ++entered_play; }};
                    while (!stop) {
                        const auto i = worker.TakeRecord();
                        if (!i) break;
                        ++taken[*i];
                        body(*i);
                    }
                } catch (const std::runtime_error&) {
                }
                ++ended;
            });
        }
    }
    void Watch() {
        crew.Watch([this] { return held.load(); },
                   [this](u32 width) {
                       kept = width;
                       ++lowered;
                   });
    }
    void Join() {
        for (auto& thread : threads) thread.join();
        threads.clear();
    }
    void ExpectEachRecordOnce() const {
        for (size_t i = 0; i < taken.size(); ++i) EXPECT_EQ(taken[i].load(), 1u) << "record " << i;
    }

    std::atomic<bool> held;
    std::atomic<bool> stop{false};
    PretranslateCrew crew;
    std::vector<std::atomic<u32>> taken;
    std::atomic<u32> entered_splash{0}, entered_play{0}, lowered{0}, kept{0}, ended{0};
    std::vector<std::thread> threads;
};

} // namespace

TEST(PretranslateCrew, WidthsComeFromTheCpuWithBounds) {
    struct {
        u32 logical, splash, play;
    } constexpr cases[]{{0, 1, 1},  {1, 1, 1},   {4, 4, 1},   {6, 6, 1},   {7, 7, 1},
                        {8, 8, 2},  {12, 12, 3}, {16, 16, 4}, {20, 20, 4}, {32, 32, 4},
                        {64, 64, 4}};
    for (const auto& c : cases) {
        EXPECT_EQ(PretranslateCrew::SplashWidth(c.logical), c.splash) << c.logical;
        EXPECT_EQ(PretranslateCrew::PlayWidth(c.logical), c.play) << c.logical;
    }
}

TEST(PretranslateCrew, ReleaseLowersAtOnceThenKeepsThePlayWidth) {
    for (const u32 logical : LogicalCpus) {
        SCOPED_TRACE(logical);
        const u32 wide = PretranslateCrew::SplashWidth(logical);
        const u32 play = PretranslateCrew::PlayWidth(logical);
        Harness h{logical, wide + 80};
        std::atomic<u32> inside{0}, running{0}, most_after{0};
        std::atomic<bool> open{false};
        h.Start([&](size_t) {
            const u32 now = ++running;
            if (open.load()) { // a record begun after the release
                for (u32 most = most_after.load(); now > most;) {
                    if (most_after.compare_exchange_weak(most, now)) break;
                }
                std::this_thread::sleep_for(1ms);
            } else {
                ++inside;
                Await([&] { return open.load(); });
            }
            --running;
        });
        EXPECT_TRUE(Await([&] { return inside.load() == wide; }));
        h.held = false;
        h.Watch();
        // Lowered while every worker is still in the middle of its record.
        EXPECT_EQ(h.lowered.load(), 1u);
        EXPECT_EQ(h.kept.load(), play);
        EXPECT_EQ(running.load(), wide);
        open = true;
        h.Join();
        EXPECT_GE(most_after.load(), 1u);
        EXPECT_LE(most_after.load(), play);
        h.ExpectEachRecordOnce();
    }
}

TEST(PretranslateCrew, ExtraWorkersLeaveBeforeTheRecordsAreDone) {
    Harness h{16, 100};
    std::atomic<u32> inside{0};
    std::atomic<bool> open{false}, finish{false};
    h.Start([&](size_t) {
        if (!open.load()) {
            ++inside;
            Await([&] { return open.load(); });
        } else {
            Await([&] { return finish.load(); });
        }
    });
    EXPECT_TRUE(Await([&] { return inside.load() == 16; }));
    h.held = false;
    h.Watch();
    open = true;
    EXPECT_TRUE(Await([&] { return h.ended.load() == 12; })); // 4 stay
    finish = true;
    h.Join();
    h.ExpectEachRecordOnce();
}

TEST(PretranslateCrew, ReleaseBeforeAWorkerStartsGivesItThePlayPolicy) {
    for (const u32 logical : LogicalCpus) {
        SCOPED_TRACE(logical);
        Harness h{logical, 64};
        h.held = false;
        h.Watch();
        std::atomic<u32> running{0}, over{0};
        h.Start([&](size_t) {
            if (++running > PretranslateCrew::PlayWidth(logical)) ++over;
            std::this_thread::yield();
            --running;
        });
        h.Join();
        EXPECT_EQ(h.entered_splash.load(), 0u);
        EXPECT_EQ(h.entered_play.load(), h.crew.Threads());
        EXPECT_EQ(over.load(), 0u);
        h.ExpectEachRecordOnce();
    }
}

TEST(PretranslateCrew, WideWhileTheHoldLasts) {
    Harness h{8, 8};
    std::atomic<u32> inside{0};
    h.Start([&](size_t) {
        ++inside;
        EXPECT_TRUE(Await([&] { return inside.load() == 8; })); // all eight at once
    });
    h.Watch(); // ends with the workers, the hold still on
    h.Join();
    EXPECT_EQ(h.lowered.load(), 0u);
    EXPECT_EQ(h.entered_splash.load(), 8u);
    h.ExpectEachRecordOnce();
}

TEST(PretranslateCrew, NeverHeldStartsInPlay) {
    Harness h{16, 32, false};
    h.Start([](size_t) {});
    h.Watch();
    h.Join();
    EXPECT_EQ(h.crew.Threads(), 4u);
    EXPECT_EQ(h.entered_play.load(), 4u);
    EXPECT_EQ(h.lowered.load(), 0u);
    h.ExpectEachRecordOnce();
}

TEST(PretranslateCrew, SwitchOffKeepsTheStartWidth) {
    Harness h{8, 8, true, false};
    h.held = false;
    h.Watch();
    std::atomic<u32> inside{0};
    h.Start([&](size_t) {
        ++inside;
        EXPECT_TRUE(Await([&] { return inside.load() == 8; }));
    });
    h.Join();
    EXPECT_EQ(h.lowered.load(), 0u);
    EXPECT_EQ(h.entered_splash.load(), 8u);
    h.ExpectEachRecordOnce();
}

TEST(PretranslateCrew, WorkersLostBeforeTheReleaseDoNotTakeTheLastOneAlong) {
    Harness h{4, 40}; // 1 in play
    std::atomic<u32> inside{0};
    std::atomic<bool> open{false};
    h.Start([&](size_t i) {
        if (i >= 4) return;
        ++inside;
        Await([&] { return inside.load() == 4; });
        if (i < 3) throw std::runtime_error{"translation stopped"};
        Await([&] { return open.load(); });
    });
    EXPECT_TRUE(Await([&] { return h.ended.load() == 3; }));
    h.held = false;
    h.Watch();
    open = true;
    h.Join();
    h.ExpectEachRecordOnce();
}

TEST(PretranslateCrew, StopEndsTheWatchWhileHeld) {
    Harness h{4, 100000};
    h.Start([&](size_t) { h.stop = true; });
    h.Watch();
    h.Join();
    EXPECT_EQ(h.lowered.load(), 0u);
}

#ifdef _WIN32
TEST(PretranslateCrew, ReleaseChangesRunningThreadsPriorityAndCores) {
    ULONG bytes = 0;
    GetSystemCpuSetInformation(nullptr, 0, &bytes, GetCurrentProcess(), 0);
    std::vector<unsigned char> sets(bytes);
    if (!bytes || !GetSystemCpuSetInformation(
                      reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(sets.data()), bytes, &bytes,
                      GetCurrentProcess(), 0)) {
        GTEST_SKIP() << "no CPU sets on this system";
    }
    const ULONG one = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(sets.data())->CpuSet.Id;
    const auto selected = [](HANDLE thread) {
        ULONG count = 0;
        GetThreadSelectedCpuSets(thread, nullptr, 0, &count);
        return count;
    };

    PretranslateCrew crew{4, true, true, 4};
    std::atomic<u32> inside{0};
    std::atomic<bool> held{true}, open{false};
    std::vector<std::thread> threads;
    for (u32 t = 0; t < crew.Threads(); ++t) {
        threads.emplace_back([&] {
            PretranslateCrew::Worker worker{
                crew,
                [&] {
                    Common::SetCurrentThreadPriority(Common::ThreadPriority::Normal);
                    // Stands for the wide set AllowAllCores selects with a core owner.
                    EXPECT_TRUE(SetThreadSelectedCpuSets(GetCurrentThread(), &one, 1));
                },
                [] {}};
            if (!worker.TakeRecord()) return;
            ++inside;
            Await([&] { return open.load(); });
        });
    }
    EXPECT_TRUE(Await([&] { return inside.load() == threads.size(); }));
    for (auto& thread : threads) {
        EXPECT_EQ(GetThreadPriority(thread.native_handle()), THREAD_PRIORITY_NORMAL);
        EXPECT_EQ(selected(thread.native_handle()), 1u);
    }
    held = false;
    crew.Watch([&] { return held.load(); },
               [&](u32) {
                   for (auto& thread : threads) Common::MoveThreadToBackground(thread);
               });
    for (auto& thread : threads) { // still inside their records
        EXPECT_EQ(GetThreadPriority(thread.native_handle()), THREAD_PRIORITY_BELOW_NORMAL);
        EXPECT_EQ(selected(thread.native_handle()), 0u);
    }
    open = true;
    for (auto& thread : threads) thread.join();
}
#endif
