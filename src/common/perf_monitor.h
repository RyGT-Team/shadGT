// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <string>
#if defined(_WIN32) || defined(__x86_64__)
#include <intrin.h>
#endif
#include "common/types.h"
#include "common/diagnostics.h"

namespace Common {

/// PERF-DIAG-007: CPU time used by each thread of this process since the previous call,
/// as "name pct%" entries for the busiest threads (percent of one core), plus the process total.
/// Returns an empty string where unsupported.
#ifdef SHADGT_DIAGNOSTICS
std::string SampleThreadCpuUsage(size_t max_threads);
#else
inline std::string SampleThreadCpuUsage(size_t) { return {}; }
#endif

#ifndef SHADGT_DIAGNOSTICS
// Reporting counters have no Release readers. Draws remains atomic for boot/save progress.
struct DiagnosticCounter {
    constexpr u64 operator++() noexcept { return 0; }
    constexpr void operator+=(u64) noexcept {}
    constexpr u64 fetch_add(u64, std::memory_order = std::memory_order_seq_cst) noexcept { return 0; }
    constexpr u64 load(std::memory_order = std::memory_order_seq_cst) const noexcept { return 0; }
};
#else
using DiagnosticCounter = std::atomic<u64>;
#endif

/// PERF-DIAG-010: work done by the emulator's GPU frontend, read and reset by the monitor.
struct WorkCounters {
    DiagnosticCounter pm4_packets{};
    std::atomic<u64> draws{};
    DiagnosticCounter dispatches{};
    DiagnosticCounter find_image{};
    DiagnosticCounter obtain_buffer{};
    DiagnosticCounter obtain_stream{};
    DiagnosticCounter uploads{};
    DiagnosticCounter upload_bytes{};
    DiagnosticCounter protects{};
    DiagnosticCounter protect_bytes{};
    DiagnosticCounter shaders_compiled{};
    DiagnosticCounter pipelines_compiled{};
    DiagnosticCounter upload_epochs{};
    DiagnosticCounter waits_skipped{};
    // DIAG-033: where the time of a long frame went (cumulative, like the counters above).
    DiagnosticCounter gpu_waits{};
    DiagnosticCounter gpu_wait_us{};
    DiagnosticCounter frontend_waits{};
    DiagnosticCounter frontend_wait_us{};
    DiagnosticCounter pipeline_waits{};
    DiagnosticCounter pipeline_wait_us{};
    DiagnosticCounter file_reads{};
    DiagnosticCounter file_read_bytes{};
    DiagnosticCounter file_read_us{};
};
#ifdef SHADGT_DIAGNOSTICS
WorkCounters& GetWorkCounters();
#else
inline WorkCounters& GetWorkCounters() {
    static WorkCounters counters;
    return counters;
}
#endif

/// Experimental-branch switch: false when the PERF id (e.g. 14 for PERF-014) is listed in the
/// SHADGT_DISABLE_PERF environment variable (comma separated), for A/B runs without rebuilding.
bool PerfFeatureEnabled(u32 id);

/// PERF-DIAG-011: command thread time per step of a draw, in TSC ticks, reported every 2 s.
enum class Phase : u32 {
    DrawSetup,
    Pipeline,
    RenderTargets,
    VertexIndex,
    Buffers,
    Textures,
    TextureRebind,
    BeginRendering,
    Descriptors,
    DynamicState,
    Record,
    DrawTotal,
    DispatchTotal,
    Submit,
    // PERF-040: parts of the steps above (counted in them as well).
    TexFindImage,
    TexView,
    TexTransit,
    BufObtain,
    ReadAheadEval,
    ProgramMatch,
    SelectAhead, ///< PERF-047: pipeline selection on the command thread (not part of draws)
    // DIAG-119: finer parts of textures, buffers and vertex/index binding.
    TexPass1,
    TexSamplers,
    BufDirect,
    VtxBind,
    IdxBind,
    SelFlatten, ///< DIAG-121: RefreshFlatBuf during program matching
    SelSpec,    ///< DIAG-121: StageSpecialization built during program matching
    Count,
};
#ifdef SHADGT_DIAGNOSTICS
std::array<std::atomic<u64>, size_t(Phase::Count)>& GetPhaseTicks();
#endif

// Functional frame cadence for compile admission, retained when diagnostics are disabled.
struct GameFrameTotals { u64 frames; u64 us; };
void NoteGameFrameCadence();
GameFrameTotals GetGameFrameTotals();

/// DIAG-033: called for every presented game frame. A frame that took longer than
/// SHADGT_STALL_MS (default 100) is logged with what the emulator did during it.
#ifdef SHADGT_DIAGNOSTICS
void NoteGameFrame();
#else
inline void NoteGameFrame() noexcept {}
#endif

// PERF-026: the draw-step timers run about 14 times per draw, so the counter read and the
// tick array are inline instead of function calls.
#ifdef SHADGT_DIAGNOSTICS
inline std::array<std::atomic<u64>, size_t(Phase::Count)> g_phase_ticks{};
#endif

/// SHADGT_LEAN=1: no per-draw timing (PhaseTimer) or sampled selection checks (PERF-047),
/// for the best frame rate a build gives; the 2 s lines then show zero step times.
#ifdef SHADGT_DIAGNOSTICS
bool LeanRun();
#else
inline constexpr bool LeanRun() noexcept { return true; }
#endif

class PhaseTimer {
public:
#ifdef SHADGT_DIAGNOSTICS
    explicit PhaseTimer(Phase phase_) : phase{phase_}, start{Enabled() ? ReadTsc() : 0} {}
    ~PhaseTimer() {
        if (start != 0) {
            g_phase_ticks[size_t(phase)].fetch_add(ReadTsc() - start, std::memory_order_relaxed);
        }
    }
    static bool Enabled() {
        static const bool enabled = !LeanRun();
        return enabled;
    }
    static u64 ReadTsc() {
#if defined(_WIN32) || defined(__x86_64__)
        return __rdtsc();
#else
        return ReadTscFallback();
#endif
    }
    static u64 ReadTscFallback();

private:
    Phase phase;
    u64 start;
#else
    explicit constexpr PhaseTimer(Phase) noexcept {}
    static constexpr bool Enabled() noexcept { return false; }
    static constexpr u64 ReadTsc() noexcept { return 0; }
    static constexpr u64 ReadTscFallback() noexcept { return 0; }
#endif
};
#ifdef SHADGT_DIAGNOSTICS
std::string TakeWorkCounters();
#else
inline std::string TakeWorkCounters() { return {}; }
#endif

} // namespace Common
