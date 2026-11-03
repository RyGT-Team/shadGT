// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace Vulkan {
// Optional promotion only: a missing result always leaves the original guest
// buffer usable. One slot bounds queued + executing + prepared storage to 256 MiB.
// Allocation and abandoned-result destruction never hold the handoff mutex.
// Prepare must return a blank, fixed-usage allocation whose compatibility is
// determined solely by exact byte size and generation. Request addresses may
// be used for speculative warming, never for populating or owning its content.
template <class Buffer>
class AsyncMirrorBuffer {
public:
    struct Request {
        std::uint64_t begin, end, generation;
        bool operator==(const Request&) const = default;
    };
    enum class Observation { None, Busy, Queued, Pending, Ready, Reused,
                             DiscardSize, DiscardGeneration };
    using Prepare = std::function<std::unique_ptr<Buffer>(Request)>;
    explicit AsyncMirrorBuffer(Prepare prepare, std::function<void()> setup = {})
        : prepare(std::move(prepare)), worker([this, setup = std::move(setup)] {
              if (setup) setup();
              Run();
          }) {}
    ~AsyncMirrorBuffer() {
        { std::scoped_lock lock(mutex); stopping = true; }
        cv.notify_one();
        worker.join();
    }
    // No wait, allocation, driver call or buffer destruction on the caller.
    std::unique_ptr<Buffer> TryAcquire(Request next, Observation* observation = nullptr) {
        if (observation) *observation = Observation::None;
        if (next.end <= next.begin || next.end - next.begin > (256ull << 20)) return {};
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock() || stopping) {if(observation)*observation=Observation::Busy;return {};}
        if (state == State::Idle) {
            request = next;
            state = State::Queued;
            if(observation)*observation=Observation::Queued;
            lock.unlock();
            cv.notify_one();
            return {};
        }
        if (state == State::Ready) {
            if (request.generation == next.generation &&
                request.end - request.begin == next.end - next.begin) {
                state = State::Idle;
                if(observation)*observation=request.begin==next.begin?Observation::Ready:Observation::Reused;
                return std::move(result);
            }
            if(observation)*observation=request.generation!=next.generation?Observation::DiscardGeneration:Observation::DiscardSize;
            // Different mapping/range: only the worker may destroy the old result.
            state = State::Discard;
            lock.unlock();
            cv.notify_one();
        }
        else if(observation)*observation=Observation::Pending;
        return {};
    }
    // Queue only an idle slot. Never consume or replace an existing result.
    // The recorder charges budget only while holding this lock and only for a new request.
    template <class Start> bool TryPrepare(Request next, Start&& start) {
        if (next.end <= next.begin || next.end - next.begin > (256ull << 20)) return false;
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock() || stopping || state != State::Idle || !start()) return false;
        request = next;
        state = State::Queued;
        lock.unlock();
        cv.notify_one();
        return true;
    }
    AsyncMirrorBuffer(const AsyncMirrorBuffer&) = delete;
    AsyncMirrorBuffer& operator=(const AsyncMirrorBuffer&) = delete;
private:
    enum class State { Idle, Queued, Running, Ready, Discard };
    void Run() {
        for (;;) {
            Request current{};
            std::unique_ptr<Buffer> discard;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return stopping || state == State::Queued ||
                                             state == State::Discard; });
                if (stopping) {
                    discard = std::move(result);
                    lock.unlock();
                    return; // discard destroyed here, on worker, outside the mutex
                }
                if (state == State::Discard) {
                    discard = std::move(result);
                    // Keep slot occupied until destruction has actually finished.
                } else {
                    current = request;
                    state = State::Running;
                }
            }
            if (discard) {
                discard.reset();
                std::scoped_lock lock(mutex);
                state = State::Idle;
                continue;
            }
            // Prepare failure is retried by a later request, never synchronously.
            std::unique_ptr<Buffer> prepared;
            try { prepared = prepare(current); } catch (...) {}
            {
                std::scoped_lock lock(mutex);
                result = std::move(prepared);
                state = result ? State::Ready : State::Idle;
            }
        }
    }
    Prepare prepare;
    std::mutex mutex;
    std::condition_variable cv;
    Request request{};
    State state = State::Idle;
    std::unique_ptr<Buffer> result;
    bool stopping = false;
    std::thread worker; // last: everything accessed by Run is initialized first
};
} // namespace Vulkan
