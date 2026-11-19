// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// FEAT-SGT-022 (SHADGT_RECORDER_BINDER=1): a thread that makes the native recorder's descriptor
// binds (PushBindings: descriptor keys, the set tables, set writes, the bind commands), about
// three quarters of what the recorder does after a draw's bindings are worked out (DIAG-SGT-023).
//
// The recorder fills a job and records a placeholder into its command ring where the binds go.
// The binder runs the jobs in order and records each one's commands into its own ring. When the
// command thread reaches a placeholder it waits for that job, then replays the binder's commands
// there (as MT-002's worker chunks), so the commands keep the order they had.
//
// Ordering rules the recorder keeps:
// - A job holds pointers into the recorder's state (the bindings, the stream). The recorder does
//   not reuse a bindings slot before its jobs are done, and drains the binder before it changes
//   anything else a job reads (splices, stream swaps, evictions, dropped samplers).
// - Values that can change between draws are copied into the job when it is made.

#include <array>
#include <atomic>
#include <cstdint>
#include <stop_token>
#include <thread>

#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_command_stream.h"

namespace Native {

template <typename Job>
class RecorderBinder {
public:
    using RunFn = void (*)(void* owner, Job& job, Vulkan::CommandRing& ring);
    static constexpr std::uint64_t Slots = 64;          ///< jobs made and not yet run
    static constexpr std::uint64_t EndSlots = 1u << 14; ///< jobs run and not yet replayed

    RecorderBinder(void* owner_, RunFn run_) : owner{owner_}, run{run_} {
        thread = std::jthread([this](std::stop_token stop) { Loop(stop); });
    }

    ~RecorderBinder() {
        thread.request_stop();
        wake.fetch_add(1, std::memory_order_release);
        wake.notify_one();
    }

    RecorderBinder(const RecorderBinder&) = delete;
    RecorderBinder& operator=(const RecorderBinder&) = delete;

    /// Recorder: the next job to fill (waits while the binder or the command thread is too far
    /// behind for its slots).
    Job& Next() {
        const std::uint64_t seq = made.load(std::memory_order_relaxed) + 1;
        while (seq - done.load(std::memory_order_acquire) > Slots ||
               seq - replayed.load(std::memory_order_acquire) > EndSlots) {
            Pause();
        }
        return jobs[seq % Slots];
    }

    /// Recorder: hands over the job Next() returned; its number.
    std::uint64_t Push() {
        const std::uint64_t seq = made.fetch_add(1, std::memory_order_relaxed) + 1;
        published.store(seq, std::memory_order_seq_cst);
        if (sleeping.load(std::memory_order_seq_cst)) {
            wake.fetch_add(1, std::memory_order_release);
            wake.notify_one();
        }
        return seq;
    }

    /// Recorder: the number of the last job made.
    std::uint64_t Made() const noexcept {
        return made.load(std::memory_order_relaxed);
    }

    /// Waits until every job made so far has run.
    void Drain() const {
        WaitDone(made.load(std::memory_order_relaxed));
    }

    /// Waits until the command thread replayed every job made so far (before the binder goes).
    void WaitReplayed() const {
        const std::uint64_t last = made.load(std::memory_order_relaxed);
        while (replayed.load(std::memory_order_acquire) < last) {
            std::this_thread::yield();
        }
    }

    /// Any thread: whether job `seq` has run.
    bool Done(std::uint64_t seq) const noexcept {
        return done.load(std::memory_order_acquire) >= seq;
    }

    /// Any thread: waits until job `seq` has run.
    void WaitDone(std::uint64_t seq) const {
        std::uint32_t turns = 0;
        while (done.load(std::memory_order_acquire) < seq) {
            if (++turns < 4096) {
                Pause();
            } else {
                std::this_thread::yield();
            }
        }
    }

    /// Command thread, after WaitDone(seq): where job seq's commands end in Ring().
    std::uint64_t End(std::uint64_t seq) const noexcept {
        return ends[seq % EndSlots];
    }

    /// Command thread: job seq's commands were replayed (its end slot may be reused).
    void Replayed(std::uint64_t seq) noexcept {
        replayed.store(seq, std::memory_order_release);
    }

    Vulkan::CommandRing& Ring() noexcept {
        return ring;
    }

private:
    static void Pause() {
#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#else
        std::this_thread::yield();
#endif
    }

    void Loop(std::stop_token stop) {
        Common::SetCurrentThreadName("shadGT:Binder");
        std::uint64_t next = 1;
        while (!stop.stop_requested()) {
            if (published.load(std::memory_order_acquire) < next) {
                // The recorder hands jobs over steadily while it works: spin before sleeping.
                std::uint32_t turns = 0;
                while (published.load(std::memory_order_acquire) < next && ++turns < 8192) {
                    Pause();
                }
                if (published.load(std::memory_order_acquire) < next) {
                    const std::uint32_t seen = wake.load(std::memory_order_acquire);
                    sleeping.store(true, std::memory_order_seq_cst);
                    if (published.load(std::memory_order_seq_cst) < next &&
                        !stop.stop_requested()) {
                        wake.wait(seen, std::memory_order_acquire);
                    }
                    sleeping.store(false, std::memory_order_relaxed);
                }
                continue;
            }
            run(owner, jobs[next % Slots], ring);
            ends[next % EndSlots] = ring.Recorded();
            done.store(next, std::memory_order_release);
            ++next;
        }
    }

    void* owner;
    RunFn run;
    std::array<Job, Slots> jobs{};
    std::array<std::uint64_t, EndSlots> ends{};
    Vulkan::CommandRing ring;
    std::atomic<std::uint64_t> made{0}; ///< written by the recorder only
    alignas(64) std::atomic<std::uint64_t> published{0};
    alignas(64) std::atomic<std::uint64_t> done{0};
    alignas(64) std::atomic<std::uint64_t> replayed{0};
    alignas(64) std::atomic<std::uint32_t> wake{0};
    std::atomic<bool> sleeping{false};
    std::jthread thread; ///< last: started after everything above, stopped first
};

} // namespace Native
