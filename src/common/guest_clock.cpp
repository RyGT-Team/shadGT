// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <fmt/format.h>

#include "common/guest_clock.h"
#include "common/logging/log.h"
#include "common/perf_monitor.h"
#include "common/rdtsc.h"

namespace Common::GuestClock {

namespace {

PausableCounter& Counter() {
    static PausableCounter counter;
    return counter;
}

thread_local bool is_gpu_command_thread = false;

std::atomic<u64> long_holds{};
std::atomic<double> total_ms{};

// DIAG-GTS-518: per site, the holds of 1 ms or more and their time; a summary every 10 s.
constexpr std::array<const char*, size_t(HoldSite::Count)> kSiteNames{
    "other",         "lookup translations", "required build", "build wait",
    "compute",       "shader on GPU thread", "translation job", "retranslate"};
std::array<std::atomic<u64>, size_t(HoldSite::Count)> site_holds{};
std::array<std::atomic<u64>, size_t(HoldSite::Count)> site_us{};
std::array<std::atomic<u64>, size_t(HoldSite::Count)> site_max_us{};
std::atomic<s64> site_report_ms{0};

void NoteSite(HoldSite site, u64 us) {
    if (us < 1000) {
        return;
    }
    const size_t i = size_t(site);
    site_holds[i].fetch_add(1, std::memory_order_relaxed);
    site_us[i].fetch_add(us, std::memory_order_relaxed);
    u64 max = site_max_us[i].load(std::memory_order_relaxed);
    while (us > max && !site_max_us[i].compare_exchange_weak(max, us, std::memory_order_relaxed)) {
    }
    const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 last = site_report_ms.load(std::memory_order_relaxed);
    if (last == 0) {
        site_report_ms.compare_exchange_strong(last, now, std::memory_order_relaxed);
        return;
    }
    if (now - last < 10000 || !site_report_ms.compare_exchange_strong(last, now)) {
        return;
    }
    std::string text;
    for (size_t s = 0; s < kSiteNames.size(); ++s) {
        const u64 n = site_holds[s].exchange(0, std::memory_order_relaxed);
        const u64 t = site_us[s].exchange(0, std::memory_order_relaxed);
        const u64 m = site_max_us[s].exchange(0, std::memory_order_relaxed);
        if (n != 0) {
            text += fmt::format("{}{} {} ({:.0f} ms, longest {:.0f})", text.empty() ? "" : ", ",
                                kSiteNames[s], n, t / 1000.0, m / 1000.0);
        }
    }
    LOG_WARNING(Common, "DIAG-GTS-518: guest clocks held in the last {} s, by site: {}",
                (now - last) / 1000, text);
}

} // namespace

bool Enabled() {
    static const bool enabled = PerfFeatureEnabled(45);
    return enabled;
}

u64 ReadTsc() {
    if (!Enabled()) {
        return FencedRDTSC();
    }
    return Counter().Read([] { return FencedRDTSC(); });
}

bool IsPaused() {
    return Enabled() && Counter().IsPaused();
}

void MarkGpuCommandThread() {
    is_gpu_command_thread = true;
}

void TakeHold() {
    if (Enabled()) {
        Counter().Pause(FencedRDTSC());
    }
}

void ReleaseHold() {
    if (Enabled()) {
        Counter().Resume(FencedRDTSC());
    }
}

namespace {
struct StartGate {
    std::mutex mutex;
    std::condition_variable opened;
    bool closed = false;
};
StartGate& Gate() {
    static StartGate gate;
    return gate;
}
} // namespace

void CloseStartGate() {
    std::scoped_lock lock{Gate().mutex};
    Gate().closed = true;
}

void OpenStartGate() {
    {
        std::scoped_lock lock{Gate().mutex};
        if (!Gate().closed) {
            return;
        }
        Gate().closed = false;
    }
    Gate().opened.notify_all();
}

bool WaitStartGate(std::chrono::seconds limit) {
    std::unique_lock lock{Gate().mutex};
    if (!Gate().closed) {
        return false;
    }
    if (!Gate().opened.wait_for(lock, limit, [] { return !Gate().closed; })) {
        // Never left closed for good: the precompile's own limit is the same (900 s).
        Gate().closed = false;
        LOG_ERROR(Common, "FIX-SGT-134: the start gate stayed closed {} s; the game goes on",
                  limit.count());
    }
    return true;
}

CompilePause::CompilePause(HoldSite site_) : active{is_gpu_command_thread && Enabled()}, site{site_} {
    if (active) {
        start = std::chrono::steady_clock::now();
        Counter().Pause(FencedRDTSC());
    }
}

CompilePause::~CompilePause() {
    if (!active) {
        return;
    }
    const auto held = std::chrono::steady_clock::now() - start;
    NoteSite(site, u64(std::chrono::duration_cast<std::chrono::microseconds>(held).count()));
    if (Counter().Resume(FencedRDTSC()) == 0) {
        return; // An outer hold continues.
    }
    // The longer holds are the ones the game would otherwise have seen as long frames.
    const double ms = std::chrono::duration<double, std::milli>(held).count();
    total_ms += ms;
    if (ms >= 50.0) {
        const u64 count = ++long_holds;
        if (count <= 40 || count % 100 == 0) {
            LOG_WARNING(Common,
                        "FIX-044: guest clocks held {:.0f} ms while the GPU thread compiled "
                        "({} holds of 50 ms or more, {:.0f} ms held in all)",
                        ms, count, total_ms.load());
        }
    }
}

} // namespace Common::GuestClock
