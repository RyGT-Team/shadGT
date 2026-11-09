// Debug-only, bounded normal-play queue tracing. No guest memory is read here.
#pragma once
#ifndef NDEBUG
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <span>
#include <cstdint>
#include <type_traits>

namespace GeometryTrace {
using u64 = std::uint64_t;
struct Event { u64 ns, id, a, b, c, d; const char* kind; };
struct State {
    std::mutex mutex;
    std::array<Event, 16384> events{};
    size_t count{};
    size_t head{};
    u64 dropped{};
    std::atomic<u64> next{1};
    std::atomic<bool> armed{};
    bool triggered{};
    size_t post_events{};
    std::atomic<bool> sealed{};
};
inline State& Get() { static State state; return state; }
inline bool Enabled() { static const bool enabled = [] { const char* p = std::getenv("RYGT_GEOMETRY_TRACE"); return p && p[0] == '1' && !p[1]; }(); return enabled; }
inline thread_local u64 current{};
template <typename T>
inline u64 HandleIdentity(T value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<std::uintptr_t>(value);
    else return static_cast<u64>(value);
}
inline u64 Hash(std::span<const std::uint32_t> words) {
    u64 hash = 14695981039346656037ull;
    // Hash only a bounded CPU prefix; this is not a GPU buffer-content hash.
    for (auto word : words.first(std::min<std::size_t>(words.size(), 256))) {
        hash ^= word; hash *= 1099511628211ull;
    }
    return hash;
}
inline void Record(const char* kind, u64 id, u64 a=0, u64 b=0, u64 c=0, u64 d=0) {
    if (!Enabled() || !Get().armed.load(std::memory_order_relaxed)) return;
    auto& s = Get();
    std::scoped_lock lock{s.mutex};
    if (!s.armed.load(std::memory_order_relaxed) || s.sealed.load(std::memory_order_relaxed)) return;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (s.count == s.events.size()) ++s.dropped; else ++s.count;
    s.events[s.head] = {u64(ns),id,a,b,c,d,kind};
    s.head = (s.head + 1) % s.events.size();
    if (s.triggered && ++s.post_events >= 1024) s.sealed.store(true, std::memory_order_release);
}
inline void Trigger() {
    if (!Enabled()) return;
    auto& s=Get(); std::scoped_lock lock{s.mutex};
    if (s.armed.load(std::memory_order_relaxed) && !s.triggered) s.triggered=true;
}
// Caller holds the state lock. A failed open, write, or close leaves the
// sealed in-memory events intact so a later poll can retry.
inline bool WriteCsv(const State& s, const char* path) {
    auto* f = std::fopen(path, "w");
    if (!f) return false;
    bool ok = std::fprintf(f,"kind,ns,id,a,b,c,d\n") >= 0;
    const size_t first = s.count == s.events.size() ? s.head : 0;
    for (size_t i=0; i<s.count && ok; ++i) { const auto& e=s.events[(first+i)%s.events.size()];
        ok = std::fprintf(f,"%s,%llu,%llu,%llu,%llu,%llu,%llu\n",e.kind,
            (unsigned long long)e.ns,(unsigned long long)e.id,(unsigned long long)e.a,
            (unsigned long long)e.b,(unsigned long long)e.c,(unsigned long long)e.d) >= 0; }
    if (ok) ok = std::fprintf(f,"dropped,0,0,%llu,0,0,0\n",(unsigned long long)s.dropped) >= 0;
    return std::fclose(f) == 0 && ok;
}
// Create geometry-trace.arm, then geometry-trace.flush in the working directory.
// Poll once per 1024 decoded jobs. The trace is one-shot; restart to arm again.
inline void PollFlush() {
    if (!Enabled()) return;
    static std::atomic<u64> polls{};
    if ((++polls & 1023) != 0) return;
    static std::atomic<bool> flushed{};
    if (flushed.load(std::memory_order_acquire)) return;
    std::error_code ec;
    if (!Get().armed.load(std::memory_order_relaxed)) {
        if (std::filesystem::exists("geometry-trace.arm", ec) && !ec)
            Get().armed.store(true, std::memory_order_release);
        return;
    }
    if (!Get().sealed.load(std::memory_order_acquire) &&
        (!std::filesystem::exists("geometry-trace.flush", ec) || ec)) return;
    auto& s = Get();
    std::scoped_lock lock{s.mutex};
    if (flushed.load(std::memory_order_relaxed)) return;
    s.sealed.store(true, std::memory_order_release);
    if (WriteCsv(s, "geometry-trace.csv")) {
        s.armed.store(false, std::memory_order_release);
        flushed.store(true, std::memory_order_release);
    }
}
struct Scope { u64 previous; explicit Scope(u64 id):previous(current){current=id;} ~Scope(){current=previous;} };
}
#endif
