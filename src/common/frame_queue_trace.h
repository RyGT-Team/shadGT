// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
namespace Common::FrameQueueTrace {
using U64 = unsigned long long;
struct Ticket {
    U64 generation{}, id{}, submit{}, prepare_begin{}, enqueue{}, dequeue{};
};
struct Row {
    Ticket ticket{};
    U64 flip_begin{}, present_begin{}, present_end{}, wait_begin{}, wait_end{}, lock_attempt{},
        lock_acquired{}, swapchain_return{}, ready_semaphore{}, ready_tick{}, frame_id{},
        present_outcome{};
    U64 draws{}, dispatches{}, shaders{}, pipelines{}, uploads{}, upload_bytes{}, protects{},
        protect_bytes{};
};
inline bool CompleteRow(const Row& r, U64 generation) noexcept {
    return (r.ticket.generation & 1) && r.ticket.generation == generation && r.ticket.enqueue &&
           r.ticket.submit && r.ticket.submit <= r.ticket.prepare_begin &&
           r.ticket.prepare_begin <= r.ticket.enqueue && r.ticket.enqueue <= r.ticket.dequeue &&
           r.ticket.dequeue <= r.flip_begin && r.flip_begin <= r.present_begin &&
           r.present_begin <= r.present_end;
}
// Multiple producers use a try-only gate; consumer is the sole reader.
template <std::size_t N>
class Ring {
    std::array<Row, N> data{};
    std::atomic<U64> head{}, tail{};
    std::atomic_flag gate = ATOMIC_FLAG_INIT;

public:
    bool Push(const Row& row) noexcept {
        if (gate.test_and_set(std::memory_order_acquire))
            return false;
        const U64 h = head.load(std::memory_order_relaxed);
        bool ok = h - tail.load(std::memory_order_acquire) < N;
        if (ok) {
            data[h % N] = row;
            head.store(h + 1, std::memory_order_release);
        }
        gate.clear(std::memory_order_release);
        return ok;
    }
    bool Pop(Row& row) noexcept {
        const U64 t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire))
            return false;
        row = data[t % N];
        tail.store(t + 1, std::memory_order_release);
        return true;
    }
};
void Initialize() noexcept;
#ifdef FRAME_QUEUE_TRACE_TEST
void PauseSinkForTest(bool) noexcept;
bool SinkIsPausedForTest() noexcept;
#endif
Ticket Start() noexcept;
void Prepare(Ticket&) noexcept;
void Enqueue(Ticket&) noexcept;
void Dequeue(Ticket&) noexcept;
U64 Now(const Ticket&) noexcept;
bool Valid(const Ticket&) noexcept;
void Complete(const Row&) noexcept;
extern thread_local Row* current;
struct Scope {
    Row* prior;
    explicit Scope(Row* r) noexcept : prior(current) {
        current = r;
    }
    ~Scope() {
        current = prior;
    }
};
} // namespace Common::FrameQueueTrace
