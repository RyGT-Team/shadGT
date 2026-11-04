#pragma once
#include "ryty_gpu/native/slow_call_trace.h"
namespace Native {
#ifndef NDEBUG
template <size_t N>
class UploadPhaseTrace {
    using Clock = std::chrono::steady_clock;
public:
    UploadPhaseTrace(bool enabled, const char* operation, std::array<const char*, N> names)
        : enabled{enabled}, operation{operation}, names{names} {
        if (enabled) {
            cpu_start = CpuTime();
            start = previous = Clock::now();
        }
    }
    void Next(size_t next) {
        if (!enabled) return;
        Account(Clock::now());
        phase = next;
    }
    void Next(size_t next, uint64_t address, uint64_t size) {
        Next(next);
        context_address = address;
        context_size = size;
    }
    ~UploadPhaseTrace() {
        if (!enabled) return;
        const auto end = Clock::now();
        Account(end);
        const double total = std::chrono::duration<double, std::milli>(end - start).count();
        if (total < 1.0) return;
        const double cpu_ms = double(CpuTime() - cpu_start) / 10000.0;
        static thread_local Clock::time_point last_log{};
        if (total < 5.0 && end - last_log < std::chrono::milliseconds(100)) return;
        last_log = end;
        std::array<double, N> ms{};
        for (size_t i = 0; i < N; ++i) ms[i] = double(ns[i]) / 1e6;
        LOG_WARNING(Render_Vulkan,
            "NAT-TRACE-UPLOAD: {} {:.2f} ms; longest {} {:.2f} ms at {:#x}+{:#x}; phases [{}] ms [{}]; thread-cpu {:.3f} ms",
            operation, total, names[longest_phase], double(longest_ns) / 1e6,
            longest_address, longest_size, fmt::join(names, "/"), fmt::join(ms, "/"), cpu_ms);
    }
    UploadPhaseTrace(const UploadPhaseTrace&) = delete;
    UploadPhaseTrace& operator=(const UploadPhaseTrace&) = delete;
private:
    static uint64_t CpuTime() {
#ifdef _WIN32
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
            return (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                   (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
        }
#endif
        return 0;
    }
    void Account(Clock::time_point now) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous).count();
        ns[phase] += elapsed;
        if (elapsed > longest_ns) {
            longest_ns = elapsed;
            longest_phase = phase;
            longest_address = context_address;
            longest_size = context_size;
        }
        previous = now;
    }
    bool enabled;
    const char* operation;
    std::array<const char*, N> names;
    std::array<int64_t, N> ns{};
    Clock::time_point start{}, previous{};
    size_t phase{}, longest_phase{};
    int64_t longest_ns{};
    uint64_t context_address{}, context_size{}, longest_address{}, longest_size{};
    uint64_t cpu_start{};
};
#else
template <size_t N>
class UploadPhaseTrace {
public:
    constexpr UploadPhaseTrace(bool, const char*, std::array<const char*, N>) noexcept {}
    constexpr void Next(size_t) noexcept {}
    constexpr void Next(size_t, uint64_t, uint64_t) noexcept {}
    UploadPhaseTrace(const UploadPhaseTrace&) = delete;
    UploadPhaseTrace& operator=(const UploadPhaseTrace&) = delete;
};
#endif

}
