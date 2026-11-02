// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include "common/frame_queue_trace.h"
#include "common/thread.h"
#ifdef _WIN32
#include <windows.h>
#endif
namespace Common::FrameQueueTrace {
thread_local Row* current = nullptr;
namespace {
U64 Clock() noexcept {
#ifdef _WIN32
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return U64(t.QuadPart);
#else
    return U64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
#endif
}
#ifdef FRAME_QUEUE_TRACE_TEST
std::atomic<bool> pause_sink{}, sink_paused{};
#endif
struct State {
    Ring<8192> ring;
    std::atomic<U64> generation{}, sequence{}, drops{}, partial{};
    std::jthread writer;
    State() noexcept {
        try {
            const char* enabled = std::getenv("RYGT_FRAME_QUEUE_TRACE");
            const char* output = std::getenv("RYGT_FRAME_QUEUE_TRACE_OUTPUT");
            const char* control = std::getenv("RYGT_FRAME_QUEUE_TRACE_CONTROL");
            const char* status = std::getenv("RYGT_FRAME_QUEUE_TRACE_STATUS");
            if (!enabled || std::strcmp(enabled, "1") || !output || !control || !status)
                return;
            writer = std::jthread([this, out = std::string(output), ctl = std::string(control),
                                   stat = std::string(status)](std::stop_token stop) {
                Common::SetCurrentThreadName("shadGT:FrameTrace");
                Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
                FILE* file = std::fopen(out.c_str(), "wb");
                U64 frequency = 1000000000, pid = 0, start_filetime = 0;
#ifdef _WIN32
                LARGE_INTEGER f{};
                QueryPerformanceFrequency(&f);
                frequency = U64(f.QuadPart);
                pid = GetCurrentProcessId();
                FILETIME created{}, exited{}, kernel{}, user{};
                if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
                    start_filetime = (U64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
#endif
                if (file)
                    std::fputs(
                        "pid,generation,id,submit_qpc,prepare_begin_qpc,enqueue_qpc,dequeue_qpc,"
                        "flip_begin_qpc,present_begin_qpc,present_end_qpc,wait_submitted_begin_qpc,"
                        "wait_submitted_end_qpc,submit_lock_attempt_qpc,submit_lock_acquired_qpc,"
                        "swapchain_present_return_qpc,ready_semaphore,ready_tick,frame_id,present_"
                        "outcome,draws_cumulative_overlap,dispatches_cumulative_overlap,shaders_"
                        "cumulative_overlap,pipelines_cumulative_overlap,uploads_cumulative_"
                        "overlap,upload_bytes_cumulative_overlap,protects_cumulative_overlap,"
                        "protect_bytes_cumulative_overlap\n",
                        file);
                bool active = false;
                U64 ack = Clock();
                while (true) {
                    bool wanted = active;
                    if (FILE* c = std::fopen(ctl.c_str(), "rb")) {
                        const int value = std::fgetc(c), end = std::fgetc(c);
                        if ((value == '0' || value == '1') &&
                            (end == EOF || end == '\n' || end == '\r'))
                            wanted = value == '1';
                        std::fclose(c);
                    }
                    if (wanted != active) {
                        active = wanted;
                        const auto g = generation.load(std::memory_order_relaxed);
                        generation.store((g + 2) & ~U64{1} | U64(active),
                                         std::memory_order_release);
                        ack = Clock();
                    }
#ifdef FRAME_QUEUE_TRACE_TEST
                    if (pause_sink.load(std::memory_order_acquire)) {
                        sink_paused.store(true, std::memory_order_release);
                        while (pause_sink.load(std::memory_order_acquire) && !stop.stop_requested())
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        sink_paused.store(false, std::memory_order_release);
                    }
#endif
                    Row r;
                    while (ring.Pop(r))
                        if (!CompleteRow(r, generation.load(std::memory_order_acquire)))
                            partial.fetch_add(1, std::memory_order_relaxed);
                        else if (file)
                            std::fprintf(file,
                                         "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%"
                                         "llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%"
                                         "llu,%llu,%llu,%llu,%llu\n",
                                         pid, r.ticket.generation, r.ticket.id, r.ticket.submit,
                                         r.ticket.prepare_begin, r.ticket.enqueue, r.ticket.dequeue,
                                         r.flip_begin, r.present_begin, r.present_end, r.wait_begin,
                                         r.wait_end, r.lock_attempt, r.lock_acquired,
                                         r.swapchain_return, r.ready_semaphore, r.ready_tick,
                                         r.frame_id, r.present_outcome, r.draws, r.dispatches,
                                         r.shaders, r.pipelines, r.uploads, r.upload_bytes,
                                         r.protects, r.protect_bytes);
                        else
                            drops.fetch_add(1, std::memory_order_relaxed);
                    if (file)
                        std::fflush(file);
                    if (FILE* f = std::fopen(stat.c_str(), "wb")) {
                        std::fprintf(f,
                                     "schema=1\ngeneration=%llu\nactive=%u\nack_qpc=%llu\nqpc_"
                                     "frequency=%llu\npid=%llu\nprocess_start_filetime=%"
                                     "llu\ndropped=%llu\npartial=%llu\noutput_ok=%u\n",
                                     generation.load(), unsigned(active), ack, frequency, pid,
                                     start_filetime, drops.load(), partial.load(),
                                     unsigned(file && !std::ferror(file)));
                        std::fclose(f);
                    }
                    if (stop.stop_requested())
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
                if (file)
                    std::fclose(file);
            });
        } catch (...) {
        }
    }
};
State& Get() {
    static State s;
    return s;
}
std::atomic<State*> initialized{};
} // namespace
void Initialize() noexcept {
    initialized.store(&Get(), std::memory_order_release);
}
#ifdef FRAME_QUEUE_TRACE_TEST
void PauseSinkForTest(bool pause) noexcept {
    pause_sink.store(pause, std::memory_order_release);
}
bool SinkIsPausedForTest() noexcept {
    return sink_paused.load(std::memory_order_acquire);
}
#endif
Ticket Start() noexcept {
    auto* state = initialized.load(std::memory_order_acquire);
    if (!state)
        return {};
    auto& s = *state;
    const U64 g = s.generation.load(std::memory_order_acquire);
    if (!(g & 1))
        return {};
    Ticket t{};
    t.generation = g;
    t.submit = Clock();
    if (s.generation.load(std::memory_order_acquire) != g)
        return {};
    return t;
}
void Prepare(Ticket& t) noexcept {
    if (t.generation)
        t.prepare_begin = Now(t);
}
void Enqueue(Ticket& t) noexcept {
    auto* state = initialized.load(std::memory_order_acquire);
    if (!state) {
        t = {};
        return;
    }
    if (!Valid(t) || !t.submit || !t.prepare_begin) {
        if (t.generation)
            state->partial.fetch_add(1, std::memory_order_relaxed);
        t = {};
        return;
    }
    auto& s = *state;
    t.id = s.sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    t.enqueue = Now(t);
    if (!t.enqueue) {
        s.partial.fetch_add(1, std::memory_order_relaxed);
        t = {};
    }
}
bool Valid(const Ticket& t) noexcept {
    const auto* state = initialized.load(std::memory_order_acquire);
    return state && t.generation &&
           state->generation.load(std::memory_order_acquire) == t.generation;
}
U64 Now(const Ticket& t) noexcept {
    if (!Valid(t))
        return 0;
    const auto now = Clock();
    return Valid(t) ? now : 0;
}
void Dequeue(Ticket& t) noexcept {
    if (t.generation)
        t.dequeue = Now(t);
}
void Complete(const Row& row) noexcept {
    auto* state = initialized.load(std::memory_order_acquire);
    if (!state)
        return;
    if (!CompleteRow(row, state->generation.load(std::memory_order_acquire))) {
        state->partial.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!state->ring.Push(row))
        state->drops.fetch_add(1, std::memory_order_relaxed);
}
} // namespace Common::FrameQueueTrace
