// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifndef NDEBUG
#include <atomic>
#include <cstdint>
#include <cstdlib>
namespace AmdGpu::ReadAheadPair {
inline bool Enabled() noexcept {
    static const bool enabled = [] { const char* v = std::getenv("RYGT_READ_AHEAD_KEY_TRACE"); return v && v[0] == '1' && v[1] == '\0'; }();
    return enabled;
}
inline std::atomic<std::uint64_t> next{1};
inline std::atomic<std::uint64_t> recorder_started{};
inline void NoteRecorderStarted(std::uint64_t token) noexcept {
    auto current = recorder_started.load(std::memory_order_relaxed);
    while (current < token && !recorder_started.compare_exchange_weak(
                                  current, token, std::memory_order_release,
                                  std::memory_order_relaxed)) {}
}
inline thread_local std::uint64_t current{};
inline thread_local bool recorder{};
inline std::uint64_t NewToken() noexcept { return Enabled() ? next.fetch_add(1, std::memory_order_relaxed) : 0; }
class Scope {
public:
    Scope(std::uint64_t token, bool is_recorder) noexcept : old(current), old_recorder(recorder) { current = token; recorder = is_recorder; }
    ~Scope() { current = old; recorder = old_recorder; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    std::uint64_t old; bool old_recorder;
};
struct Metadata { std::uint64_t token{}; bool indirect{}; };
}
#endif
