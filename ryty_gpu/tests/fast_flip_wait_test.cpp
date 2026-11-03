// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace Common {
void AccurateSleep(Clock::duration d, void*, bool) { std::this_thread::sleep_for(d); }
struct GuestClock {
    static inline std::atomic<bool> paused{};
    static bool IsPaused() { return paused.load(); }
};
}
struct DebugStateStub {
    std::atomic<bool> paused{};
    bool IsGuestThreadsPaused() const { return paused.load(); }
} DebugState;
struct Stats { std::mutex mutex; unsigned early_flips{}; } g_flip_stats;
#define FRAME_END ((void)0)

struct Harness {
    std::mutex mutex, seen_mutex;
    std::condition_variable request_cv, seen_cv;
    std::queue<int> requests;
    std::vector<int> seen;
    std::atomic<int> rate{}, ticks{}, paused_redraws{}, overdue_ticks{};
    bool fast = true;
    Clock::duration flip_work{};

    void Enqueue(int n) {
        { std::scoped_lock lock{mutex}; requests.push(n); }
        request_cv.notify_one();
    }
    void Run(std::stop_token token, Clock::duration tick_period, Clock::time_point next_tick) {
        const unsigned ticks_per_vblank = fast ? 4 : 1;
        const auto current_flip_rate = [&] { return rate.load(); };
#include "fast-flip-eligibility-under-test.inc"
        const auto receive_request = [&] {
            std::scoped_lock lock{mutex};
            if (requests.empty()) return 0;
            auto r = requests.front(); requests.pop(); return r;
        };
        const auto Flip = [&](int r) {
            if (flip_work != Clock::duration{}) std::this_thread::sleep_for(flip_work);
            { std::scoped_lock lock{seen_mutex}; seen.push_back(r); }
            seen_cv.notify_all();
        };
        while (!token.stop_requested()) {
#include "fast-flip-wait-under-test.inc"
            // Model the following production pause/tick branch; actual wait and
            // eligibility above are source-extracted.
            ++ticks;
            if (overdue) ++overdue_ticks;
            if (DebugState.IsGuestThreadsPaused() || Common::GuestClock::IsPaused()) {
                ++paused_redraws;
            } else if (current_flip_rate() == 0) {
                if (const auto request = receive_request()) Flip(request);
            }
            seen_cv.notify_all();
        }
    }
    size_t SeenCount() { std::scoped_lock lock{seen_mutex}; return seen.size(); }
    bool WaitSeen(size_t count, Clock::duration timeout) {
        std::unique_lock lock{seen_mutex};
        return seen_cv.wait_for(lock, timeout, [&] { return seen.size() >= count; });
    }
    void Stop(std::jthread& thread) {
        { std::scoped_lock lock{mutex}; thread.request_stop(); }
        request_cv.notify_all();
        thread.join();
    }
};

int main() {
    Harness fifo;
    fifo.Enqueue(1); fifo.Enqueue(2);
    auto deadline = Clock::now() + 400ms;
    std::jthread t([&](std::stop_token token) { fifo.Run(token, 400ms, deadline); });
    assert(fifo.WaitSeen(2, 250ms));
    assert(fifo.seen == std::vector<int>({1, 2}) && fifo.ticks == 0);
    fifo.Stop(t);

    // All four eligibility conditions reject early dequeue without spinning.
    for (int mode = 0; mode < 4; ++mode) {
        Harness blocked;
        blocked.fast = mode != 0;
        blocked.rate = mode == 1 ? 1 : 0;
        DebugState.paused = mode == 2;
        Common::GuestClock::paused = mode == 3;
        blocked.Enqueue(3);
        auto due = Clock::now() + 150ms;
        std::jthread thread([&](std::stop_token token) { blocked.Run(token, 150ms, due); });
        std::this_thread::sleep_for(40ms);
        assert(blocked.ticks == 0 && blocked.SeenCount() == 0);
        blocked.Stop(thread);
        DebugState.paused = false;
        Common::GuestClock::paused = false;
    }

    // Eligibility changes after wait begins but before the queued wake.
    Harness stale;
    std::jthread stale_thread([&](std::stop_token token) { stale.Run(token, 180ms, Clock::now()+180ms); });
    std::this_thread::sleep_for(20ms);
    DebugState.paused = true;
    stale.Enqueue(4);
    std::this_thread::sleep_for(35ms);
    assert(stale.SeenCount() == 0 && stale.ticks == 0);
    std::this_thread::sleep_for(160ms);
    assert(stale.paused_redraws >= 1 && stale.SeenCount() == 0);
    DebugState.paused = false;
    assert(stale.WaitSeen(1, 300ms));
    stale.Stop(stale_thread);

    Harness cross;
    cross.flip_work = 90ms; cross.Enqueue(5);
    std::jthread t3([&](std::stop_token token) { cross.Run(token, 100ms, Clock::now()+50ms); });
    assert(cross.WaitSeen(1, 300ms));
    std::this_thread::sleep_for(20ms);
    assert(cross.ticks >= 1 && cross.overdue_ticks >= 1);
    cross.Stop(t3);

    Harness debt;
    std::jthread t4([&](std::stop_token token) { debt.Run(token, 100ms, Clock::now()-250ms); });
    std::this_thread::sleep_for(30ms);
    assert(debt.ticks >= 3);
    debt.Stop(t4);

    Harness late;
    std::jthread t5([&](std::stop_token token) { late.Run(token, 500ms, Clock::now()+500ms); });
    std::this_thread::sleep_for(20ms); late.Enqueue(6);
    assert(late.WaitSeen(1, 200ms)); late.Stop(t5);

    // Original accumulated debt and absolute deadline recurrence, including
    // initial tick, work time, sleep undershoot, overshoot, and catch-up.
    long long old_debt=0, now=0, deadline_ns=0;
    for (auto [work, sleep] : std::vector<std::pair<long long,long long>>{{2,0},{4,8},{1,15},{3,4},{0,11}}) {
        old_debt -= sleep; now += sleep;
        assert((old_debt < 0) == (now > deadline_ns));
        old_debt += 10-work; deadline_ns += 10; now += work;
    }
}
