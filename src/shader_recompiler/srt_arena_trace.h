// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifndef NDEBUG
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>

namespace Shader::SrtArenaTrace {
enum class Origin : unsigned { Other, Speculative, Required, Synchronous, Flags, CachedRebuild, Count };
static_assert(unsigned(Origin::Count) == 6, "Update the trace report when adding an origin");
struct Counters {
    std::atomic<std::uint64_t> generated{}, generated_count{}, partial{}, partial_count{};
    std::atomic<std::uint64_t> cache_requests{}, cache_bytes{}, cache_hits{}, cache_avoided{};
    std::atomic<std::uint64_t> cache_misses{}, cache_allocated{}, cache_partial{}, zero_key{}, mismatch{};
    std::array<std::atomic<std::uint64_t>, unsigned(Origin::Count)> success{}, failure{}, success_bytes{}, failure_bytes{};
};
inline Counters counters;
inline thread_local Origin origin = Origin::Other;
inline thread_local std::uint64_t generated_bytes = 0;
inline bool Enabled() {
    static const bool enabled = [] { const char* v = std::getenv("RYGT_SRT_ARENA_TRACE"); return v && v[0] == '1' && v[1] == '\0'; }();
    return enabled;
}
class OriginScope {
public:
    explicit OriginScope(Origin next) : old(origin) { if (Enabled()) origin = next; }
    ~OriginScope() { origin = old; }
    OriginScope(const OriginScope&) = delete;
    OriginScope& operator=(const OriginScope&) = delete;
private:
    Origin old;
};
class FrontendScope {
public:
    FrontendScope() : active(Enabled()), before(generated_bytes), exceptions(std::uncaught_exceptions()), category(origin) {}
    FrontendScope(const FrontendScope&) = delete;
    FrontendScope& operator=(const FrontendScope&) = delete;
    ~FrontendScope() {
        if (!active) return;
        const auto i = unsigned(category);
        if (std::uncaught_exceptions() > exceptions) {
            ++counters.failure[i]; counters.failure_bytes[i] += generated_bytes - before;
        } else {
            ++counters.success[i]; counters.success_bytes[i] += generated_bytes - before;
        }
    }
private:
    bool active;
    std::uint64_t before;
    int exceptions;
    Origin category;
};
void Report(std::uint64_t used, const char* reason) noexcept;
} // namespace Shader::SrtArenaTrace
#endif
