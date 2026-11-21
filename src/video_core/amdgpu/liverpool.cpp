#include "common/debug_environment.h"
#include "common/geometry_trace.h"
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h> // PERF-SGT-028: _mm_pause
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <boost/preprocessor/stringize.hpp>
#include <fmt/ranges.h>

#include "common/assert.h"
#include "common/diagnostics.h"
#include "common/sampling_profiler.h"
#include "common/scope_exit.h"
#include "common/debug.h"
#include "common/guest_clock.h"
#include "common/mapping_writeback_gate.h"
#include "common/nvtx.h"
#include "common/perf_monitor.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include <immintrin.h> // FEAT-SGT-079: _mm_pause
#include "common/native_thread_priority.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/videoout/driver.h"
#include "core/memory.h"
#include "core/platform.h"
#include "video_core/amdgpu/ce_de_counter.h"
#include "video_core/amdgpu/draw_pipe.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include <xxhash.h>

#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/amdgpu/pm4_rewind.h"
#include "video_core/amdgpu/pm4_type0.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "core/libraries/system/systemservice.h"
#include "ryty_gpu/ryty_diag.h"
#ifdef RYTY_GPU
#include "ryty_gpu/native/shadow.h"
#include "video_core/renderer_vulkan/vk_rasterizer_native.h"
#endif

namespace AmdGpu {

// A fence signaled after the GPU completes may target memory the guest unmapped meanwhile.
// PERF-DIAG-012: when deferred fences last wrote each address, to tell waits on GPU completion
// from waits on the CPU or another queue.
static std::mutex g_deferred_fence_writes_mutex;
static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point> g_deferred_fence_writes;

// DIAG-055: graphics end-of-pipe interrupts and the game's next submission.
static std::atomic<s64> g_last_eop_ns{0};
static std::atomic<u64> g_eops_deferred{0};
static std::atomic<u64> g_eops_immediate{0};

// EXP-063 (measurement only, not safe for play): graphics EOP/EOS fences are signaled when the
// command thread decodes them instead of after the recorder has recorded the work before them.
// It measures how fast races get when the game no longer waits for the recorder. The game may
// then rewrite memory the recorder has not read yet, and reads GPU results before they exist,
// so rendering can be wrong. On while the file named by SHADGT_EARLY_FENCES_FLAG exists (checked
// every 250 ms), so a benchmark can turn it on after the menus. Command thread only.
static std::atomic<u64> g_fences_early{0};
static bool EarlyFencesActive() {
    static const char* const flag = std::getenv("SHADGT_EARLY_FENCES_FLAG");
    if (!flag || !*flag) {
        return false;
    }
    static std::chrono::steady_clock::time_point next_check{};
    static bool active = false;
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_check) {
        next_check = now + std::chrono::milliseconds(250);
        std::error_code ec;
        const bool on = std::filesystem::exists(flag, ec);
        if (on != active) {
            LOG_WARNING(Render, "EXP-063: early fences {} (measurement only)", on ? "on" : "off");
        }
        active = on;
    }
    return active;
}
static s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
static void CheckIbAtDecode(const u32* data, u32 dwords, u64 decoding); // DIAG-062
static std::atomic<u64> g_eop_submit{0}; // DIAG-066: submission of the last EOP signaled
static std::atomic<u64> g_eop_count{0};
// DIAG-SGT-074 (SHADGT_DIAG_EOP_LATENCY=1, diagnostics builds): is the frame gated by our latency?
// SubmitGfx stamps each graphics submission; an EOP interrupt signaled for submission N measures
// N's submit-to-signal time; the next submission after a signal measures the game's reaction.
// Every 5 s: counts, averages and maxima, and the submissions between two signals.
static bool EopLatencyDiag() {
    if constexpr (!Common::DiagnosticsBuilt) {
        return false;
    } else {
        static const bool on = Common::DiagEnv("SHADGT_DIAG_EOP_LATENCY") != nullptr;
        return on;
    }
}
static std::array<std::atomic<s64>, 1024> g_submit_ns{}; // by submission number
// By submission number: when the command processor decoded its EOP (0) and when the recorder
// reached it (1); with the submit and the signal, the chain's three parts.
static std::array<std::array<std::atomic<s64>, 1024>, 2> g_eop_stage_ns{};
// The scheduler tick the EOP's work went into (noted when the recorder reached it): its submit
// splits the tail into our part (reached -> submitted) and the GPU's (submitted -> signal).
static std::array<std::atomic<u64>, 1024> g_eop_tick{};
static std::array<std::array<s64, 3>, 2> g_arm_tail{}; // by arm: count, to submit, to signal
// DIAG-SGT-080: the command processor's idle sleeps (no queue had work), split by whether a game
// submission was still undecoded when it slept (its EOP not decoded yet): then decode waited for
// the game, not for us.
static std::atomic<s64> g_last_submit_ns{0}, g_last_decoded_ns{0};
static std::atomic<s64> g_cp_idle_ns{0}, g_cp_idle_open_ns{0}, g_cp_idle_sleeps{0},
    g_cp_idle_open_sleeps{0};
static std::atomic<s64> g_lat_last_eop_ns{0};
static std::mutex g_lat_mutex;
static u64 g_lat_eops = 0, g_lat_submits = 0, g_lat_reactions = 0, g_lat_split = 0;
static s64 g_lat_sum = 0, g_lat_max = 0, g_react_sum = 0, g_react_max = 0, g_lat_since = 0;
static std::array<s64, 3> g_part_sum{}, g_part_max{};
// The same split by the arm of an A/B in turns (SubmitAbArm), when one runs.
static std::array<u64, 2> g_arm_split{};
static std::array<std::array<s64, 4>, 2> g_arm_sum{}; // the three parts, then submit -> signal
static void NoteEopStage(u64 submit, int stage) {
    if (!EopLatencyDiag() || submit == 0) return;
    const s64 now = NowNs();
    g_eop_stage_ns[stage][submit % 1024].store(now, std::memory_order_relaxed);
    if (stage == 0) g_last_decoded_ns.store(now, std::memory_order_relaxed); // DIAG-SGT-080
}
static void ReportEopLatency(s64 now) {
    if (g_lat_since == 0) {
        g_lat_since = now;
        return;
    }
    if (now - g_lat_since < 5'000'000'000) return;
    LOG_INFO(Render, "DIAG-SGT-074: in 5 s: {} graphics submits, {} EOP interrupts ({:.1f} submits "
                     "each); submit to EOP signal avg {:.2f} ms max {:.2f} ms; EOP signal to the "
                     "next submit ({} times) avg {:.2f} ms max {:.2f} ms",
             g_lat_submits, g_lat_eops,
             g_lat_eops ? double(g_lat_submits) / double(g_lat_eops) : 0.0,
             g_lat_eops ? double(g_lat_sum) / double(g_lat_eops) / 1e6 : 0.0, g_lat_max / 1e6,
             g_lat_reactions,
             g_lat_reactions ? double(g_react_sum) / double(g_lat_reactions) / 1e6 : 0.0,
             g_react_max / 1e6);
    if (g_lat_split) {
        const auto avg = [](s64 sum) { return double(sum) / double(g_lat_split) / 1e6; };
        LOG_INFO(Render, "DIAG-SGT-074: submit to EOP split ({} EOPs): submit -> decoded avg "
                         "{:.2f} max {:.2f} ms; decoded -> recorder reached it avg {:.2f} max "
                         "{:.2f} ms; recorder -> signal (GPU tail and fence) avg {:.2f} max {:.2f} ms",
                 g_lat_split, avg(g_part_sum[0]), g_part_max[0] / 1e6, avg(g_part_sum[1]),
                 g_part_max[1] / 1e6, avg(g_part_sum[2]), g_part_max[2] / 1e6);
    }
    if (g_arm_split[0] && g_arm_split[1]) {
        std::string arms;
        for (int a = 0; a < 2; ++a) {
            const auto avg = [&](int k) { return double(g_arm_sum[a][k]) / double(g_arm_split[a]) / 1e6; };
            arms += fmt::format(" [{}: {} EOPs, submit -> signal {:.2f} ms: decode {:.2f}, recorder "
                                "trail {:.2f}, tail {:.2f}]",
                                a ? "B" : "A", g_arm_split[a], avg(3), avg(0), avg(1), avg(2));
        }
        LOG_INFO(Render, "DIAG-SGT-074: by arm of the A/B:{}", arms);
    }
    {
        std::string tails;
        for (int a = 0; a < 2; ++a) {
            const auto& t = g_arm_tail[a];
            if (t[0] == 0) continue;
            tails += fmt::format(" [{}{}: {} EOPs, recorder reached -> list submitted {:.2f} ms, "
                                 "submitted -> signal {:.2f} ms]",
                                 g_arm_split[0] && g_arm_split[1] ? "arm " : "",
                                 g_arm_split[0] && g_arm_split[1] ? (a ? "B" : "A") : "all", t[0],
                                 double(t[1]) / double(t[0]) / 1e6, double(t[2]) / double(t[0]) / 1e6);
        }
        if (!tails.empty()) LOG_INFO(Render, "DIAG-SGT-074: tail split:{}", tails);
        if (const s64 sleeps = g_cp_idle_sleeps.exchange(0); sleeps != 0 && g_lat_eops != 0) {
            const double per = 1e6 * double(g_lat_eops);
            LOG_INFO(Render, "DIAG-SGT-080: command processor idle {:.2f} ms an EOP ({} sleeps), "
                             "of it {:.2f} ms ({} sleeps) while a submission's EOP was not decoded",
                     double(g_cp_idle_ns.exchange(0)) / per, sleeps,
                     double(g_cp_idle_open_ns.exchange(0)) / per, g_cp_idle_open_sleeps.exchange(0));
        }
        if (const auto first = ::Vulkan::TakeFirstListStats(); first[0] != 0) {
            LOG_INFO(Render, "DIAG-SGT-074: the frame's first list reached the queue {:.2f} ms "
                             "(max {:.2f}) after the game's first submit ({} frames)",
                     double(first[1]) / double(first[0]) / 1e6, double(first[2]) / 1e6, first[0]);
        }
        g_arm_tail = {};
    }
    g_arm_split = {};
    for (auto& arm : g_arm_sum) arm = {};
    g_lat_eops = g_lat_submits = g_lat_reactions = g_lat_split = 0;
    g_part_sum = {};
    g_part_max = {};
    g_lat_sum = g_lat_max = g_react_sum = g_react_max = 0;
    g_lat_since = now;
}
static void NoteSubmitForLatency(u64 submit) {
    if (!EopLatencyDiag()) return;
    const s64 now = NowNs();
    g_submit_ns[submit % g_submit_ns.size()].store(now, std::memory_order_relaxed);
    g_last_submit_ns.store(now, std::memory_order_relaxed); // DIAG-SGT-080
    std::scoped_lock lk{g_lat_mutex};
    ++g_lat_submits;
    if (const s64 eop = g_lat_last_eop_ns.exchange(0); eop != 0) {
        ::Vulkan::NoteFrameStart(now); // the frame's first submit
        const s64 react = now - eop;
        ++g_lat_reactions;
        g_react_sum += react;
        g_react_max = std::max(g_react_max, react);
    }
    ReportEopLatency(now);
}
static void NoteEopForLatency(u64 submit) {
    if (!EopLatencyDiag() || submit == 0) return;
    const s64 now = NowNs();
    const s64 at = g_submit_ns[submit % g_submit_ns.size()].load(std::memory_order_relaxed);
    g_lat_last_eop_ns.store(now);
    std::scoped_lock lk{g_lat_mutex};
    if (at != 0 && now > at) {
        ++g_lat_eops;
        g_lat_sum += now - at;
        g_lat_max = std::max(g_lat_max, now - at);
        const s64 decoded = g_eop_stage_ns[0][submit % 1024].load(std::memory_order_relaxed);
        const s64 reached = g_eop_stage_ns[1][submit % 1024].load(std::memory_order_relaxed);
        if (decoded >= at && reached >= decoded && now >= reached) {
            const std::array<s64, 3> parts{decoded - at, reached - decoded, now - reached};
            ++g_lat_split;
            const int arm = ::Vulkan::SubmitAbArm().load(std::memory_order_relaxed);
            const u64 tick = g_eop_tick[submit % 1024].load(std::memory_order_relaxed);
            const s64 submitted = tick ? ::Vulkan::TickSubmittedNs(tick) : 0;
            if (submitted >= reached && now >= submitted) {
                ++g_arm_tail[arm][0];
                g_arm_tail[arm][1] += submitted - reached;
                g_arm_tail[arm][2] += now - submitted;
            }
            ++g_arm_split[arm];
            g_arm_sum[arm][3] += now - at;
            for (int i = 0; i < 3; ++i) g_arm_sum[arm][i] += parts[i];
            for (int i = 0; i < 3; ++i) {
                g_part_sum[i] += parts[i];
                g_part_max[i] = std::max(g_part_max[i], parts[i]);
            }
        }
    }
    ReportEopLatency(now);
}
static void NoteEop(u64 submit = 0) {
    NoteEopForLatency(submit); // DIAG-SGT-074
#ifndef NDEBUG
    g_last_eop_ns.store(NowNs());
    g_eop_submit.store(submit);
    ++g_eop_count;
    Common::Nvtx::Mark("EOP interrupt"); // DIAG-056
#endif
}

static void WriteDeferredFence(void* address, const void* data, u32 num_bytes) {
#ifndef NDEBUG
    {
        std::scoped_lock lk{g_deferred_fence_writes_mutex};
        if (g_deferred_fence_writes.size() > 4096) {
            g_deferred_fence_writes.clear();
        }
        g_deferred_fence_writes[reinterpret_cast<uintptr_t>(address)] =
            std::chrono::steady_clock::now();
    }
#endif
    auto* memory = Core::Memory::Instance();
    const auto writeback = [&] {
        if (memory->IsValidMapping(reinterpret_cast<VAddr>(address), num_bytes))
            memory->TryWriteBacking(address, data, num_bytes);
    };
#ifdef RYTY_GPU
    // PERF-GTS-359: map the page's alias view before taking the writeback gate. Ryty's map call
    // waited 635 ms at Autopolis, and the recorder waits for the gate to defer its next fence.
    Core::RytyMemory::PrepareCachedAlias(reinterpret_cast<VAddr>(address), num_bytes);
    Common::CompleteScopedMappingWriteback(writeback);
#else
    writeback();
#endif
}

// PERF-095 (opt-in, SHADGT_LAZY_ASC_EPOCH=1): a compute-queue submission or wait only bumps the
// upload epoch once a compute queue records something that reads memory (a dispatch or a DMA).
// On a PS4 Pro, 3 of the 9 compute submissions per race frame only fence, flush caches or chain
// empty IB links. In the October 13 race A/B it halved the epochs (8 -> 4 per frame) without a
// measurable gain (run-to-run noise ~10%), so it stays off.
static bool LazyAscEpochEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADGT_LAZY_ASC_EPOCH");
        return env != nullptr && env[0] == '1';
    }();
    return enabled;
}
static std::atomic<bool> g_asc_epoch_pending{false};
static void NoteAscEpoch() {
    if (LazyAscEpochEnabled()) {
        g_asc_epoch_pending.store(true, std::memory_order_release);
    } else {
        VideoCore::BumpUploadEpoch();
    }
}
static void BumpPendingAscEpoch() {
    if (g_asc_epoch_pending.load(std::memory_order_acquire) &&
        g_asc_epoch_pending.exchange(false, std::memory_order_acq_rel)) {
        VideoCore::BumpUploadEpoch();
    }
}

// PERF-DIAG-009: how long each queue's command processing is blocked in wait packets, and on
// which addresses, reported every 2 s. Only the command processor thread updates it.
enum class FrontendWait : u32 {
    GfxWaitRegMem,
    GfxVoLabel,
    GfxMemSemaphore,
    GfxRewind,
    AscWaitRegMem,
    AscMemSemaphore,
    AscRewind,
    Count
};
static std::chrono::steady_clock::time_point FrontendWaitNow() noexcept {
#ifndef NDEBUG
    return std::chrono::steady_clock::now();
#else
    return {};
#endif
}
// DIAG-SGT-080 (diagnostics builds, SHADGT_DIAG_READ_AHEAD_AB=1): no pipeline read-ahead (PERF-034)
// in arm 1 of the 2 s A/B, for its cost to decode against what it saves the recorder.
static bool ReadAheadSkipped() {
    static const bool ab = Common::DiagEnv("SHADGT_DIAG_READ_AHEAD_AB") != nullptr;
    return ab && ::Vulkan::SubmitAbArm().load(std::memory_order_relaxed) == 1;
}

static void RecordFrontendWait(FrontendWait kind, uintptr_t address,
                               std::chrono::steady_clock::time_point start) {
    // PERF-012: whoever the queue waited for may have written memory later draws read.
    // PERF-095: for a compute queue, only once it records a memory read.
    if (kind >= FrontendWait::AscWaitRegMem) {
        NoteAscEpoch();
    } else {
        VideoCore::BumpUploadEpoch();
    }
#ifndef NDEBUG
    static constexpr std::array<const char*, size_t(FrontendWait::Count)> Names = {
        "gfx WAIT_REG_MEM", "gfx VO label",      "gfx MEM_SEMAPHORE", "gfx REWIND",
        "asc WAIT_REG_MEM", "asc MEM_SEMAPHORE", "asc REWIND"};
    static std::array<std::pair<u32, double>, size_t(FrontendWait::Count)> totals{};
    static std::pair<u32, double> gfx_on_fence{};
    static std::unordered_map<uintptr_t, std::pair<FrontendWait, double>> addresses;
    static auto window_start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - start).count();
    ++Common::GetWorkCounters().frontend_waits;
    Common::GetWorkCounters().frontend_wait_us += u64(ms * 1000.0);
    RyDiag::RecordElapsed(RyDiag::Zone::CommandWait, now - start, address);
    auto& total = totals[size_t(kind)];
    ++total.first;
    total.second += ms;
    if (kind == FrontendWait::GfxWaitRegMem) {
        std::scoped_lock lk{g_deferred_fence_writes_mutex};
        const auto it = g_deferred_fence_writes.find(address);
        if (it != g_deferred_fence_writes.end() && it->second >= start) {
            ++gfx_on_fence.first;
            gfx_on_fence.second += ms;
        }
    }
    auto& entry = addresses[address];
    entry.first = kind;
    entry.second += ms;
    if (now - window_start < std::chrono::seconds{2}) {
        return;
    }
    std::string summary;
    for (size_t i = 0; i < Names.size(); ++i) {
        if (totals[i].first != 0) {
            summary += fmt::format(" {}={}/{:.1f}ms", Names[i], totals[i].first, totals[i].second);
        }
    }
    summary += fmt::format(" (gfx WAIT_REG_MEM ended by a deferred fence: {}/{:.1f}ms)",
                           gfx_on_fence.first, gfx_on_fence.second);
    gfx_on_fence = {};
    std::vector<std::pair<uintptr_t, std::pair<FrontendWait, double>>> top(addresses.begin(),
                                                                           addresses.end());
    std::ranges::sort(top, std::greater{}, [](const auto& e) { return e.second.second; });
    summary += ";";
    for (size_t i = 0; i < std::min<size_t>(5, top.size()); ++i) {
        summary += fmt::format(" {} {:#x} {:.1f}ms", Names[size_t(top[i].second.first)],
                               top[i].first, top[i].second.second);
    }
    LOG_WARNING(Render, "Command processor waits in {:.1f} s (count/total):{}",
                std::chrono::duration<double>(now - window_start).count(), summary);
    totals = {};
    addresses.clear();
    window_start = now;
#endif
}

// PERF-014: the bytes a fence packet writes, when they are known when it is processed.
static std::vector<u8> FenceValueBytes(DataSelect data_sel, u32 low, u64 qword) {
    std::vector<u8> bytes;
    if (data_sel == DataSelect::Data32Low) {
        bytes.resize(sizeof(u32));
        std::memcpy(bytes.data(), &low, sizeof(u32));
    } else if (data_sel == DataSelect::Data64) {
        bytes.resize(sizeof(u64));
        std::memcpy(bytes.data(), &qword, sizeof(u64));
    }
    return bytes;
}

// PERF-014: whether a WAIT_REG_MEM on memory is satisfied by the value a fence the command
// thread already processed will write once the GPU reaches it.
static bool SatisfiedByPendingFence(Vulkan::Rasterizer* rasterizer,
                                    const PM4CmdWaitRegMem* wait_reg_mem) {
    static const bool enabled = Common::PerfFeatureEnabled(14);
    // PERF-014b (id 1402, off by default): also wait while a deferred fence still brings GPU
    // data back to guest memory. In GT Sport one always does, so it never went early. The
    // exploded vertices first blamed on PERF-014 came from PERF-015.
    static const bool wait_for_readbacks = !Common::PerfFeatureEnabled(1402);
    if (!enabled || !rasterizer || wait_reg_mem->mem_space != PM4CmdWaitRegMem::MemSpace::Memory ||
        (wait_for_readbacks && rasterizer->HasReadbackFences())) {
        return false;
    }
    const auto value =
        rasterizer->PendingFenceDword(reinterpret_cast<VAddr>(wait_reg_mem->Address<u32*>()));
    if (!value || !wait_reg_mem->TestValue(*value)) {
        return false;
    }
    ++Common::GetWorkCounters().waits_skipped;
    return true;
}

static const char* dcb_task_name{"DCB_TASK"};
static const char* ccb_task_name{"CCB_TASK"};

class GpuWaitDiagnostics {
public:
    /// True every 5 s of a wait that keeps going round (its report, and the deferred fences
    /// flushed). PERF-SGT-025: made for every wait packet, so no clock is read when it is made,
    /// and in a wait only every 64th turn; the first 5 s count from the 64th turn.
    bool Ready() {
        if ((++turns & 63) != 0) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (next_report == std::chrono::steady_clock::time_point{}) {
            next_report = now + std::chrono::seconds(5);
            return false;
        }
        if (now < next_report) {
            return false;
        }
        next_report = now + std::chrono::seconds(5);
        return true;
    }

private:
    u32 turns = 0;
    std::chrono::steady_clock::time_point next_report{};
};

#define MAX_NAMES 56
static_assert(Liverpool::NumComputeRings <= MAX_NAMES);

#define NAME_NUM(z, n, name) BOOST_PP_STRINGIZE(name) BOOST_PP_STRINGIZE(n),
#define NAME_ARRAY(name, num) {BOOST_PP_REPEAT(num, NAME_NUM, name)}

static const char* acb_task_name[] = NAME_ARRAY(ACB_TASK, MAX_NAMES);

#define YIELD(name)                                                                                \
    FIBER_EXIT;                                                                                    \
    co_yield {};                                                                                   \
    FIBER_ENTER(name);

#define YIELD_CE() YIELD(ccb_task_name)
#define YIELD_GFX() YIELD(dcb_task_name)
#define YIELD_ASC(id) YIELD(acb_task_name[id])

#define RESUME(task, name)                                                                         \
    FIBER_EXIT;                                                                                    \
    task.handle.resume();                                                                          \
    FIBER_ENTER(name);

#define RESUME_CE(task) RESUME(task, ccb_task_name)
#define RESUME_GFX(task) RESUME(task, dcb_task_name)
#define RESUME_ASC(task, id) RESUME(task, acb_task_name[id])

std::array<u8, 48_KB> Liverpool::ConstantEngine::constants_heap;

bool Liverpool::DirectNestedResume() {
    static const bool on = [] {
        const char* env = std::getenv("SHADGT_DIRECT_NESTED_RESUME");
        if (env && *env) {
            return env[0] == '1';
        }
#ifdef RYTY_GPU
        return Vulkan::NativeModeRequested(); // PERF-GTS-407: on by default in native mode.
#else
        return false; // FIX-SGT-070: the standalone target has no native renderer.
#endif
    }();
    return on;
}

// DIAG-206 (SHADGT_WATCH_WRITES=<hex address>,<hex size>): GPU-command writes into that range.
static void WatchWrite(VAddr dst, u64 size, VAddr src, const char* what) {
    static const std::pair<u64, u64> watch = [] {
        const char* env = Common::DebugEnv("SHADGT_WATCH_WRITES");
        if (!env || !*env) {
            return std::pair<u64, u64>{0, 0};
        }
        char* end{};
        const u64 base = std::strtoull(env, &end, 16);
        const u64 len = end && *end == ',' ? std::strtoull(end + 1, nullptr, 16) : 0x1000;
        return std::pair<u64, u64>{base, len};
    }();
    if (watch.second == 0 || dst >= watch.first + watch.second || dst + size <= watch.first) {
        return;
    }
    static std::atomic<u32> count{};
    if (count++ < 256) {
        LOG_WARNING(Render, "DIAG-206: {} writes {:#x} size {:#x} from {:#x}", what, dst, size,
                    src);
    }
}

// FIX-035: DMA_DATA with a register source or destination address space (SAS/DAS = 1). GT
// Sport issues it on the graphics queue in the main menu; its exact effect is undocumented in
// the sources available, so the packet is reported once per destination and skipped rather
// than stopping the emulator.
static bool SkipRegisterSpaceDmaData(const PM4DmaData* dma_data, const char* queue) {
    if (dma_data->command.das == 0 && dma_data->command.sas == 0) {
        return false;
    }
    static std::mutex mutex;
    static std::unordered_set<u64> reported;
    std::scoped_lock lk{mutex};
    if (reported
            .insert(u64(dma_data->dst_addr_lo) << 8 | u64(dma_data->src_sel) << 4 |
                    u64(dma_data->dst_sel))
            .second) {
        LOG_ERROR(Render_Vulkan,
                  "FIX-035: {} DMA_DATA with register address space skipped: sas {} das {} "
                  "src_sel {} dst_sel {} src {:#x} dst {:#x} bytes {} saic {} daic {}",
                  queue, u32(dma_data->command.sas), u32(dma_data->command.das),
                  u32(dma_data->src_sel), u32(dma_data->dst_sel), dma_data->SrcAddress<u64>(),
                  dma_data->DstAddress<u64>(), dma_data->NumBytes(), u32(dma_data->command.saic),
                  u32(dma_data->command.daic));
    }
    return true;
}

static std::span<const u32> NextPacket(std::span<const u32> span, size_t offset) {
    if (offset > span.size()) {
        LOG_ERROR(Lib_GnmDriver,
                  "Packet exceeds submission at {:#x}: packet dwords={}, remaining dwords={}, "
                  "remaining words={:#010x}",
                  reinterpret_cast<uintptr_t>(span.data()), offset, span.size(),
                  fmt::join(span.first(std::min<size_t>(span.size(), 16)), " "));
        // Return empty subspan so check for next packet bails out
        return {};
    }

    return span.subspan(offset);
}

Liverpool::Liverpool() : guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    num_counter_pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
    process_thread = std::jthread{std::bind_front(&Liverpool::Process, this)};
}

Liverpool::~Liverpool() {
    process_thread.request_stop();
    process_thread.join();
    if (b_thread.joinable()) { // FEAT-SGT-079
        b_thread.request_stop();
        b_thread.join();
    }
    if (baton_thread.joinable()) { // FEAT-SGT-079 step 5
        baton_quit.store(true, std::memory_order_release);
        baton_seq.fetch_add(1, std::memory_order_acq_rel);
        baton_seq.notify_one();
        {
            std::scoped_lock lk{overlap_mutex};
        }
        overlap_cv.notify_all(); // FEAT-SGT-079 step 19
        baton_thread.join();
    }
    draw_pipe.reset();
}

// PERF-031: the draw pipe is opt-in (SHADGT_DRAW_PIPE=1) while it is measured; ryGT turns it on
// unless SHADGT_DRAW_PIPE=0, so GTSport.exe runs without a launcher. -DisablePerf 46 keeps it off.
static bool DrawPipeRequested() {
    static const bool requested = [] {
        const char* env = std::getenv("SHADGT_DRAW_PIPE");
#ifdef RYTY_GPU
        const bool on = !env || env[0] != '0';
#else
        const bool on = env && env[0] == '1';
#endif
        return on && Common::PerfFeatureEnabled(46);
    }();
    return requested;
}

void Liverpool::StartDrawPipe() {
    recorder = std::make_unique<RecorderState>();
    recorder->regs = regs;
    // Dispatches carry their queue's compute registers; there is no current queue here.
    recorder->cb_extent = last_cb_extent;
    recorder->db_extent = last_db_extent;
    select_state = std::make_unique<RecorderState>(*recorder); // PERF-SGT-077
    b_state = std::make_unique<RecorderState>(*recorder);      // FEAT-SGT-079
    if (RecorderBMode() == 4) {
        b_thread = std::jthread{[this](std::stop_token stop) { RecorderBThread(stop); }};
    }
    // PERF-SGT-077: compiles on the select thread hold the guest clocks, as on this one.
    DrawPipe::select_init = [] { Common::GuestClock::MarkGpuCommandThread(); };
    regs_dirty = {};
    // PERF-034: the pipeline read-ahead goes on with the draw pipe; -DisablePerf 49 (or 19)
    // turns it off.
    if (Common::PerfFeatureEnabled(49) && Common::PerfFeatureEnabled(19)) {
        read_ahead_states = std::make_unique<ReadAheadStates>();
        pipeline_regs_written = true;
    }
    if (const char* env = std::getenv("SHADGT_DRAW_PIPE_VERIFY")) {
        verify_interval = static_cast<u32>(std::strtoul(env, nullptr, 10));
    }
    draw_pipe = std::make_unique<DrawPipe>([this] {
        Common::SetCurrentThreadName("shadGT:GpuRecorder");
        VideoCore::BufferCache::MarkRecorderThread(); // R6
        Common::SamplingProfiler::RegisterCurrentThread("GpuRecorder"); // DIAG-048
        // FIX-044: shader and pipeline compiles on this thread hold the guest clocks too.
        Common::GuestClock::MarkGpuCommandThread();
        recorder_state = recorder.get();
#ifdef __linux__
        recorder_tid = gettid();
#endif
    }, DrawPipe::DefaultMaxPending, [this] {
        RecorderBSync(); // FEAT-SGT-079 step 19: B's handed work, before the recorder sleeps
        // A held label may be the very thing the guest waits for before
        // publishing its next command buffer. Submit it before the recorder
        // sleeps instead of requiring another draw or EOP to trigger a flush.
        if (rasterizer) {
            rasterizer->FlushDeferredFences();
#ifdef RYTY_GPU
            if (auto* native = rasterizer->Native()) native->BeginAhead();
#endif
        }
    });
    pipe_report_start = std::chrono::steady_clock::now();
    if (RecorderBOverlap()) { // FEAT-SGT-079 step 19: sync B before a job outside its pass
        draw_pipe->before_job = [this](u64 tag) {
            if (overlap_outstanding && (tag == 0 || tag != overlap_pass_tag)) RecorderBSync();
        };
    }
    if (RecorderBMode() >= 5) { // FEAT-SGT-079 step 5
        baton_thread = std::thread{[this] { RecorderBBatonThread(); }};
    }
    pipelined.store(true, std::memory_order_release);
    // PERF-063: SHADGT_EARLY_FENCES=1 signals graphics fences at decode when the work before
    // them reads guest memory only through a capture (=2: also right after fences that waited
    // for readbacks). Needs the command thread's selections as they are (PERF-061).
    if (const char* env = std::getenv("SHADGT_EARLY_FENCES");
        env && (env[0] == '1' || env[0] == '2') && Common::PerfFeatureEnabled(71) &&
        Common::PerfFeatureEnabled(69) && rasterizer && rasterizer->SelectsPipelinesAhead()) {
        early_fences_mode = env[0] - '0';
        // EXP-120 (SHADGT_EARLY_FENCES_LATE=<seconds>, measurement only): the mode starts that
        // long after start, at a graphics submission (the menus froze with it).
        if (const char* late = std::getenv("SHADGT_EARLY_FENCES_LATE"); late && *late) {
            early_fences_pending = early_fences_mode;
            early_fences_mode = 0;
            early_fences_at = std::chrono::steady_clock::now() + std::chrono::seconds(std::atoi(late));
        }
        LOG_WARNING(Render, "PERF-063: early fences on (mode {})", early_fences_mode);
    }
    LOG_WARNING(
        Render, "PERF-031: draws are recorded on a second thread (draw pipe){}{}",
        read_ahead_states ? ", pipelines read ahead (PERF-034)" : "",
        verify_interval ? fmt::format(", registers verified every {} draws", verify_interval) : "");
}

bool Liverpool::WaitTurnsEnabled() {
    // PERF-048: -DisablePerf 60 drains at once as before.
    static const bool enabled = Common::PerfFeatureEnabled(60);
    return enabled;
}

void Liverpool::Record(Common::UniqueFunction<void>&& work, const char* reason) {
    if (early_fences_mode) {
        UnsafeWindow(reason);
    }
    if (draw_pipe) {
        DrawPipe::SetPushTag(reason ? reason : "record"); // DIAG-069
        draw_pipe->Push(std::move(work));
    } else {
        work();
    }
}

void Liverpool::RecordDma(Common::UniqueFunction<void>&& work, VAddr src, u64 src_bytes,
                          VAddr dst, u64 dst_bytes) {
    const bool writes_memory = dst != 0 && dst_bytes != 0;
    // PERF-083: with the buffer stage, a copy does not stop it either (PERF-082): a planned
    // buffer the copy writes into is obtained again by the recorder. -DisablePerf 96 keeps
    // copies as barriers.
    static const bool dmas_neutral = Common::PerfFeatureEnabled(96);
    if (!early_fences_mode && draw_pipe && draw_pipe->HasStage() && dmas_neutral) {
        u64 seq = 0;
        if (writes_memory) {
            seq = labels_pushed.load(std::memory_order_relaxed) + 1;
            auto& record = label_ring[seq % label_ring.size()];
            record.address = dst;
            record.size = dst_bytes;
            record.seq.store(seq, std::memory_order_release);
            labels_pushed.store(seq, std::memory_order_release);
        }
        draw_pipe->Push(
            [this, seq, work = std::move(work)]() mutable {
                RecorderParts::Scope part{RecorderParts::Part::Dmas}; // DIAG-GT7-005
                work();
                if (seq != 0) {
                    labels_done.store(seq, std::memory_order_release);
                }
            },
            DrawPipe::JobKind::Neutral);
        return;
    }
    if (!early_fences_mode || !draw_pipe || !rasterizer) {
        Record(std::move(work), "dma");
        return;
    }
    // PERF-063: the bytes it reads are captured now like a draw's; a write to guest memory
    // the CPU may read before it is recorded keeps the window uncovered in mode 1.
    if (writes_memory) {
        // PERF-070: the CPU cannot read it before the copy is recorded.
        GuardGpuWrite(dst, dst_bytes);
    }
    if (!window_capture) {
        window_capture = TakeCapture();
    }
    if (src_bytes != 0 && !rasterizer->CaptureGuestRange(*window_capture, src, src_bytes)) {
        Record(std::move(work), "dma not captured");
        return;
    }
    // Covered by the window's capture, but a barrier for the buffer stage (it changes buffers).
    DrawPipe::SetPushTag("captured dma"); // DIAG-069
    draw_pipe->Push([this, work = std::move(work), capture = window_capture]() mutable {
        RecorderParts::Scope part{RecorderParts::Part::Dmas}; // DIAG-GT7-005
        rasterizer->SetReadCapture(capture.get());
        work();
        rasterizer->SetReadCapture(nullptr);
    });
}

void Liverpool::RecordSafe(Common::UniqueFunction<void>&& work) {
    if (draw_pipe) {
        draw_pipe->Push(std::move(work), DrawPipe::JobKind::Neutral);
    } else {
        work();
    }
}

// Captures go back to the pool when the last job reading them is done. The pool outlives
// everything (left to the process exit), so a capture released late still has it.
static std::mutex* const g_capture_pool_mutex = new std::mutex;
static std::vector<Common::ReadCapture*>* const g_capture_pool =
    new std::vector<Common::ReadCapture*>;

std::shared_ptr<Common::ReadCapture> Liverpool::TakeCapture() {
    Common::ReadCapture* capture = nullptr;
    {
        std::scoped_lock lk{*g_capture_pool_mutex};
        if (!g_capture_pool->empty()) {
            capture = g_capture_pool->back();
            g_capture_pool->pop_back();
        }
    }
    if (capture) {
        capture->Reset();
    } else {
        capture = new Common::ReadCapture(8192);
    }
    return std::shared_ptr<Common::ReadCapture>(capture, [](Common::ReadCapture* c) {
        std::scoped_lock lk{*g_capture_pool_mutex};
        if (g_capture_pool->size() < 64) {
            g_capture_pool->push_back(c);
        } else {
            delete c;
        }
    });
}

void Liverpool::UnsafeWindow(const char* reason) {
    if (window_safe) {
        window_safe = false;
#ifndef NDEBUG
        ++early_stats.unsafe[reason];
#endif
    }
}

void Liverpool::GuardGpuWrite(VAddr address, u64 size) {
    if (!early_fences_mode || !rasterizer || size == 0) {
        return;
    }
    rasterizer->GetPageManager().Guard(address, size);
    window_guards.emplace_back(address, size);
}

void Liverpool::WaitForGuard(VAddr page) {
    if (!early_fences_mode || !rasterizer || !rasterizer->GetPageManager().IsGuarded(page)) {
        return;
    }
#ifndef NDEBUG
    const auto start = std::chrono::steady_clock::now();
#endif
    while (rasterizer->GetPageManager().IsGuarded(page)) {
        if (recorder_in_wait.load(std::memory_order_acquire)) {
            // The recorder waits for memory a CPU thread writes; it may be this one.
            rasterizer->GetPageManager().DropGuards(page);
            rasterizer->NoteGuardDropped();
            break;
        }
        std::this_thread::yield();
    }
#ifndef NDEBUG
    ++guard_waits_070;
    guard_wait_us_070 += static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - start)
                                              .count());
#endif
}

void Liverpool::NewWindow() {
    if (!window_guards.empty()) {
        // PERF-070: the window's GPU writes are recorded once this runs.
        RecordSafe([this, guards = std::move(window_guards)] {
            for (const auto& [address, size] : guards) {
                rasterizer->GetPageManager().Unguard(address, size);
            }
        });
        window_guards.clear();
    }
#ifndef NDEBUG
    if (window_capture) {
        ++early_stats.windows_captured;
        early_stats.pages_captured += window_capture->NumPages();
        early_stats.full += window_capture->Full();
    }
#endif
    window_capture.reset();
    window_safe = true;
}

bool Liverpool::EarlyFenceAllowed() {
    if (!early_fences_mode || !draw_pipe || !rasterizer) {
        return false;
    }
    // DIAG-110 (SHADGT_EARLY_FENCES_AFTER=<seconds>): fences are signaled early only that long
    // after start (captures run from the start), to measure races without the menus.
    static const auto early_after = [] {
        const char* env = std::getenv("SHADGT_EARLY_FENCES_AFTER");
        return std::chrono::steady_clock::now() + std::chrono::seconds(env ? std::atoi(env) : 0);
    }();
    if (std::chrono::steady_clock::now() < early_after) {
        return false;
    }
    if (!window_safe) {
        return false; // counted by UnsafeWindow
    }
    if (fences_delivered.load(std::memory_order_acquire) !=
        fences_decoded.load(std::memory_order_relaxed)) {
        // Fences reach memory in order (FIX-049): an earlier one is still to be recorded.
    #ifndef NDEBUG
    ++early_stats.earlier_fence_pending;
#endif
        return false;
    }
    if (rasterizer->DeferredFencesPending()) {
    #ifndef NDEBUG
    ++early_stats.deferred_pending;
#endif
        return false;
    }
    if (early_fences_mode == 1) {
        // The game reads GPU results back (images queued for download, fences waiting for
        // readbacks) in the last 10 s: fences stay in step with the recorder, so it does not
        // read them before they exist (car select flickered with 2 s and no download check).
        const s64 last = rasterizer->LastReadbackFenceNs();
        if (last != 0 && NowNs() - last < 10'000'000'000) {
#ifndef NDEBUG
            ++early_stats.recent_readbacks;
#endif
            return false;
        }
    }
    return true;
}

bool Liverpool::TryEarlyLabel(VAddr address, std::span<const u8> value, u32 drain_source,
                              int irq) {
    const auto source = static_cast<Vulkan::Rasterizer::DrainSource>(drain_source);
    if (!early_fences_mode || !draw_pipe || !rasterizer || address == 0 || value.empty()) {
        return false;
    }
    // A protected page (tracked, GPU-written) keeps its write on the recorder: a flush of GPU
    // data there must come before the value.
    for (VAddr page = Common::AlignDown(address, 4096); page < address + value.size();
         page += 4096) {
        if (!rasterizer->IsUnwatchedPage(page)) {
            return false;
        }
    }
    if (!EarlyFenceAllowed()) {
        return false;
    }
    Core::Memory::Instance()->TryWriteBacking(reinterpret_cast<void*>(address), value.data(),
                                              u32(value.size()));
    VideoCore::BumpUploadEpoch();
    if (irq >= 0) {
        Platform::IrqC::Instance()->Signal(static_cast<Platform::InterruptId>(irq));
    }
    NoteEarlyFence();
    RecordSafe([this, source] { rasterizer->OnFence(source); });
    return true;
}

void Liverpool::NoteEarlyFence() {
#ifndef NDEBUG
    ++early_stats.early;
#endif
    fences_decoded.fetch_add(1, std::memory_order_relaxed);
    fences_delivered.fetch_add(1, std::memory_order_release);
    if (window_capture) {
        std::scoped_lock lk{early_windows_mutex};
        // Windows the recorder has finished are not guarded any more (their captures go back
        // to the pool).
        const u64 done = draw_pipe->FinishedJobs();
        while (!early_windows.empty() && early_windows.front().end_job <= done) {
            early_windows.pop_front();
        }
        early_windows.push_back({window_capture, draw_pipe->PushedJobs()});
    }
    NewWindow();
}

void Liverpool::WaitForEarlyFences(VAddr page) {
    if (!early_fences_mode || !draw_pipe) {
        return;
    }
    u64 wait_until = 0;
    {
        std::scoped_lock lk{early_windows_mutex};
        const u64 done = draw_pipe->FinishedJobs();
        while (!early_windows.empty() && early_windows.front().end_job <= done) {
            early_windows.pop_front();
        }
        for (const auto& window : early_windows) {
            if (window.capture->Lists(page)) {
                wait_until = std::max(wait_until, window.end_job);
            }
        }
    }
    if (wait_until == 0 || draw_pipe->FinishedJobs() >= wait_until) {
        return;
    }
    Common::Nvtx::Scope nvtx{"CPU fault: wait for early-fenced work"}; // DIAG-056
#ifndef NDEBUG
    const auto start = std::chrono::steady_clock::now();
#endif
    while (draw_pipe->FinishedJobs() < wait_until) {
        if (recorder_in_wait.load(std::memory_order_acquire)) {
            // The recorder waits for memory a CPU thread writes; it may be this one.
#ifndef NDEBUG
            ++guard_skipped_in_wait;
#endif
            break;
        }
        std::this_thread::yield();
    }
#ifndef NDEBUG
    ++guard_waits;
    guard_wait_us += static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
#endif
}

void Liverpool::ReportEarlyFences() {
#ifndef NDEBUG
    if (!early_fences_mode) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (early_report == std::chrono::steady_clock::time_point{}) {
        early_report = now;
        return;
    }
    if (now - early_report < std::chrono::seconds{2}) {
        return;
    }
    auto& st = early_stats;
    std::string unsafe;
    for (const auto& [reason, n] : st.unsafe) {
        unsafe += fmt::format(" {}={}", reason, n);
    }
    LOG_WARNING(Render,
                "PERF-063 early fences in {:.1f} s: {} signaled at decode, {} in step with the "
                "recorder (windows not covered:{}; earlier fence pending {}, deferred fences {}, "
                "recent readbacks {}); {} windows captured, {:.1f} pages each, {} full; CPU "
                "faults waited {} times, {:.1f} ms ({} not waited: recorder in a wait); "
                "PERF-070: guarded GPU writes waited for {} times, {:.1f} ms, {} guards dropped "
                "for emulator threads",
                std::chrono::duration<double>(now - early_report).count(), st.early, st.normal,
                unsafe.empty() ? " none" : unsafe, st.earlier_fence_pending, st.deferred_pending,
                st.recent_readbacks, st.windows_captured,
                st.windows_captured ? double(st.pages_captured) / st.windows_captured : 0.0,
                st.full, guard_waits.exchange(0), guard_wait_us.exchange(0) / 1000.0,
                guard_skipped_in_wait.exchange(0), guard_waits_070.exchange(0),
                guard_wait_us_070.exchange(0) / 1000.0, rasterizer->TakeGuardsDropped());
    st = {};
    early_report = now;
#endif
}

// FIX-014 (below): draws the rasterizer filters out.
static bool FilteredDraw(const Regs& state);

void Liverpool::RecordDraw(Common::UniqueFunction<void>&& draw, bool compute, bool direct_draw,
                           const IndirectSgprs* indirect) {
#ifndef NDEBUG
    const auto pair_token = !compute ? ReadAheadPair::NewToken() : 0;
    ReadAheadPair::Scope pair_scope{pair_token, false};
    GeometryTrace::PollFlush();
    const auto trace_id = GeometryTrace::Enabled() && GeometryTrace::Get().armed.load()
        ? GeometryTrace::Get().next.fetch_add(1) : 0;
    GeometryTrace::Scope trace_scope{trace_id};
    if (trace_id) GeometryTrace::Record("decode", trace_id, compute, direct_draw,
        regs.num_indices, regs.num_instances.NumInstances());
    if (trace_id && !compute) GeometryTrace::Record("decode_index", trace_id,
        regs.index_base_address.Address<VAddr>(), regs.max_index_size,
        u64(regs.index_buffer_type.index_type));
#endif
    if (!draw_pipe) {
#ifndef NDEBUG
        if (trace_id) GeometryTrace::Record("direct_begin", trace_id);
#endif
        draw();
#ifndef NDEBUG
        if (trace_id) GeometryTrace::Record("direct_end", trace_id);
#endif
        return;
    }
    const u64 ahead_sequence = read_ahead_states && !compute ? read_ahead_states->NextDrawSequence() : 0;
    // PERF-056: the delta vectors go back and forth between the threads with their capacity.
    std::vector<u32> delta = delta_recycler.Take();
    delta.clear();
    regs_dirty.Collect(std::span<const u32, Regs::NumRegs>{regs.reg_array}, delta);
    std::optional<ComputeProgram> cs;
    if (compute) {
        cs = GetCsRegs();
    }
    std::unique_ptr<Regs> expected;
    if (verify_interval != 0 && ++verify_count % verify_interval == 0) {
        expected = std::make_unique<Regs>(regs);
    }
    // PERF-047: the pipeline is selected now, on this thread, from these registers; the
    // recorder checks the selection against the memory it sees before using it.
    Vulkan::Rasterizer::SelectedPipelinePtr selected; // PERF-GT7-125
    bool prepared_draw = false;
    const void* prepared_hint = nullptr; // PERF-SGT-081
#ifdef RYTY_GPU
    // OPT-012: a draw a game job thread prepared is selected there (the recorder installs
    // that selection).
    if (!compute && direct_draw && decode_draw_packet &&
        regs.primitive_type != PrimitiveType::QuadList &&
        Vulkan::NativeRenderer::PreparedOn()) { // DIAG-GT7-003: off in A/B windows
        if (auto* native = rasterizer->Native()) {
            // PERF-SGT-081: where it is, for the recorder to ask for it a job ahead (it may be
            // freed or replaced by then: only prefetched, never read through this pointer).
            prepared_hint = native->PreparedAddress(decode_draw_packet);
            prepared_draw = prepared_hint != nullptr;
        }
    }
#endif
    const void* const job_packet = direct_draw && !compute ? decode_draw_packet : nullptr;
#ifdef RYTY_GPU
    decode_draw_packet = nullptr;
#endif
    // PERF-SGT-077 (SHADGT_SELECT_STAGE): the selection below runs on the select stage, from its
    // own copy of the registers, instead of here. Not with early fences (their capture of the
    // draw's reads needs the selection now) or the buffer stage.
    const bool defer_select = draw_pipe->HasSelectStage() && !early_fences_mode &&
                              !draw_pipe->HasStage();
    struct SelectSlot {
        std::vector<u32> delta;
        Vulkan::Rasterizer::SelectedPipelinePtr selected;
        bool indirect_selected = false;
        const void* hint = nullptr; ///< the selection, for the job's prefetch hint
    };
    std::shared_ptr<SelectSlot> slot;
    if (defer_select) {
        slot = std::make_shared<SelectSlot>();
        slot->delta = delta;
    }
    if (!defer_select && direct_draw && rasterizer->SelectsPipelinesAhead() && !prepared_draw) {
        selected = compute ? rasterizer->SelectComputePipelineAhead()
                           : rasterizer->SelectPipelineAhead();
    }
    // PERF-GT7-031: an indirect draw's pipeline is selected now too, with its parameters (its
    // arguments, in GPU memory, don't change it). In GT7's pause menu a fifth of the draws are
    // indirect, and the recorder selected each one itself under the lookup lock. Only the
    // pipeline: the draw's reads are neither hashed, captured nor staged ahead, as before.
    // SHADGT_INDIRECT_AHEAD=0: as before.
    static const bool indirect_ahead = [] {
        const char* env = std::getenv("SHADGT_INDIRECT_AHEAD");
        return !(env && env[0] == '0');
    }();
    bool indirect_selected = false;
    if (!defer_select && indirect && !compute && indirect_ahead && !selected &&
        rasterizer->SelectsPipelinesAhead()) {
        const Vulkan::DrawIndirectParams params{
            .vertex_sgpr_offset = indirect->vertex,
            .instance_sgpr_offset = indirect->instance,
            .tessellate_quads = true,
            .is_indirect = true,
        };
        selected = rasterizer->SelectPipelineAhead(&params);
        indirect_selected = selected != nullptr;
    }
#ifdef RYTY_GPU
    if (!defer_select && !compute && !indirect_selected && Native::ShadowComparing()) {
        // NAT-030: the pipeline the draw's job looked up on its thread, against this selection.
        Native::ShadowCheckPipeline(selected ? selected->pipeline : nullptr, selected != nullptr);
    }
#endif
    // PERF-062: the upload epoch this draw is decoded in; its pages are hashed now, on this
    // thread, and the recorder uses those hashes for this draw.
    const u32 decode_epoch = VideoCore::g_upload_epoch.load(std::memory_order_acquire);
    if (selected && !indirect_selected) {
        rasterizer->HashDrawPagesAhead(*selected, decode_epoch);
    }
    DrawPipe::Work select_work;
    if (defer_select) {
        // What the selection reads from this thread's state, taken now.
        std::optional<IndirectSgprs> indirect_sgprs;
        if (indirect) indirect_sgprs = *indirect;
        select_work = [this, slot, compute, direct_draw, prepared_draw, cs, indirect_sgprs,
                       cb_extent = last_cb_extent, db_extent = last_db_extent, decode_epoch] {
            auto& state = *select_state;
            RegsDelta<Regs::NumRegs>::Apply(slot->delta,
                                            std::span<u32, Regs::NumRegs>{state.regs.reg_array});
            if (cs) {
                state.cs = *cs;
            }
            state.cb_extent = cb_extent;
            state.db_extent = db_extent;
            // DrawRegs() reads these while selecting; the command thread's own come back after
            // (in the alternating mode this runs on the command thread).
            RecorderState* const previous = std::exchange(recorder_state, &state);
            SCOPE_EXIT {
                recorder_state = previous;
            };
            Vulkan::Rasterizer::SelectedPipelinePtr chosen;
            if (direct_draw && rasterizer->SelectsPipelinesAhead() && !prepared_draw) {
                chosen = compute ? rasterizer->SelectComputePipelineAhead()
                                 : rasterizer->SelectPipelineAhead();
            }
            bool chosen_indirect = false;
            if (indirect_sgprs && !compute && indirect_ahead && !chosen &&
                rasterizer->SelectsPipelinesAhead()) {
                const Vulkan::DrawIndirectParams params{
                    .vertex_sgpr_offset = indirect_sgprs->vertex,
                    .instance_sgpr_offset = indirect_sgprs->instance,
                    .tessellate_quads = true,
                    .is_indirect = true,
                };
                chosen = rasterizer->SelectPipelineAhead(&params);
                chosen_indirect = chosen != nullptr;
            }
#ifdef RYTY_GPU
            if (!compute && !chosen_indirect && Native::ShadowComparing()) {
                Native::ShadowCheckPipeline(chosen ? chosen->pipeline : nullptr, chosen != nullptr);
            }
#endif
            if (chosen && !chosen_indirect) {
                rasterizer->HashDrawPagesAhead(*chosen, decode_epoch);
            }
            slot->selected = std::move(chosen);
            slot->indirect_selected = chosen_indirect;
            slot->hint = slot->selected.get();
            std::vector<u32>{}.swap(slot->delta);
        };
    }
    // PERF-063: the pages the draw reads that the CPU can rewrite without a fault are copied
    // now; the recorder reads the copies.
    std::shared_ptr<Common::ReadCapture> capture;
    if (early_fences_mode) {
        if (selected && !indirect_selected) {
            if (!window_capture) {
                window_capture = TakeCapture();
            }
            VAddr index_address = 0;
            u64 index_size = 0;
            if (!compute && regs.index_base_address.Address<VAddr>() != 0) {
                const u32 index_bytes =
                    regs.index_buffer_type.index_type == IndexType::Index16 ? 2 : 4;
                index_address = regs.index_base_address.Address<VAddr>();
                index_size = u64(regs.max_index_size) * index_bytes;
            }
            if (rasterizer->CaptureDrawReads(*selected, *window_capture, index_address,
                                             index_size)) {
                capture = window_capture;
                for (const auto& [address, size] : rasterizer->CaptureWrites()) {
                    GuardGpuWrite(address, size);
                }
            } else {
                UnsafeWindow(rasterizer->CaptureFailure());
            }
        } else if (direct_draw && !compute && FilteredDraw(regs)) {
            // Clears, resolves, depth copies: GPU images only, no guest memory read.
        } else {
            UnsafeWindow(!direct_draw ? (compute ? "indirect dispatch" : "indirect draw")
                         : compute ? "dispatch not selected ahead"
                                   : "draw not selected ahead");
        }
    }
    // PERF-067: with the buffer stage, the selection and the buffers obtained for it are shared
    // by the stage's work and the recorder's.
    struct StagedDraw {
        Vulkan::Rasterizer::SelectedPipelinePtr selected;
        Vulkan::Rasterizer::BufferPlan plan;
    };
    std::shared_ptr<StagedDraw> staged;
    DrawPipe::Prepare prepare;
    // MT-005: an indexed direct draw's index buffer, as BindIndexBuffer obtains it.
    VAddr staged_index_address = 0;
    u32 staged_index_bytes = 0;
    if (staged_index_offset && !compute) {
        const u32 index_size = regs.index_buffer_type.index_type == IndexType::Index16 ? 2 : 4;
        staged_index_address = regs.index_base_address.Address<VAddr>() +
                               u64(*staged_index_offset) * index_size;
        staged_index_bytes = regs.num_indices * index_size;
    }
    staged_index_offset.reset();
    if (draw_pipe->HasStage() && selected && !indirect_selected) {
        staged = std::make_shared<StagedDraw>();
        staged->selected = std::move(selected);
        selected.reset();
        prepare = [this, staged, staged_index_address, staged_index_bytes] {
            return rasterizer->PrepareBuffersAhead(*staged->selected, staged->plan,
                                                   staged_index_address, staged_index_bytes);
        };
    }
    // A draw whose buffers the stage does not obtain is obtained by the recorder: the stage
    // must not run past it.
    // EXP-122 (SHADGT_FILTERED_NEUTRAL=1): clears, resolves and depth copies (filtered draws,
    // no pipeline selected) do not stop the buffer stage.
    static const bool filtered_neutral = [] {
        const char* env = std::getenv("SHADGT_FILTERED_NEUTRAL");
        return env && env[0] == '1';
    }();
    const bool neutral_filtered =
        filtered_neutral && !staged && direct_draw && !compute && FilteredDraw(regs);
    const auto kind = staged || neutral_filtered ? DrawPipe::JobKind::Neutral
                                                 : DrawPipe::JobKind::Barrier;
#ifndef NDEBUG
    if (trace_id) GeometryTrace::Record("queue", trace_id, decode_epoch,
        bool(staged || selected), prepared_draw, bool(capture));
    if (trace_id) GeometryTrace::Record("queue_flags", trace_id,
        u64(kind == DrawPipe::JobKind::Neutral), compute, direct_draw, bool(staged));
#endif
    if (!staged) {
        // DIAG-069
        DrawPipe::SetPushTag(compute ? (direct_draw ? "dispatch not selected" : "indirect dispatch")
                                     : (direct_draw ? "draw not selected" : "indirect draw"));
    }
    DrawPipe::Work order;
    if (staged) {
        order = [this, staged] { rasterizer->OrderBuffersAhead(*staged->selected, staged->plan); };
    }
    // PERF-GT7-126: what the job reads first through its pointers, for the recorder to ask for
    // one job ahead: the selection's stages and the register delta.
    const void* const selection_hint = staged ? static_cast<const void*>(staged->selected.get())
                                              : static_cast<const void*>(selected.get());
    // FEAT-SGT-079: what recorder B needs to work on this job ahead (B on only).
    std::shared_ptr<QueuedDraw> queued_draw;
    if (RecorderBOn()) {
        queued_draw = std::make_shared<QueuedDraw>();
        queued_draw->seq = ++queued_draw_seq;
        if (b_thread.joinable()) {
            queued_draw->delta = std::move(delta); // read by both recorders, owned by neither
        } else {
            queued_draw->delta = delta;
        }
        queued_draw->cs = cs;
        queued_draw->cb_extent = last_cb_extent;
        queued_draw->db_extent = last_db_extent;
        queued_draw->packet = job_packet;
        queued_draw->compute = compute;
        queued_draw->direct = direct_draw;
        // The deal: a pass starts where the targets differ from the draw before (a run of
        // dispatches is one pass); runs of 4 passes go in turns to A and B.
        u64 tag = 1;
        if (!compute) {
            tag = XXH3_64bits(&regs.color_buffers, sizeof(regs.color_buffers));
            tag = XXH3_64bits_withSeed(&regs.depth_buffer, sizeof(regs.depth_buffer), tag) | 2;
        }
        const bool pass_start = tag != push_key;
        if (pass_start) {
            ++push_pass;
            push_key = tag;
        }
        queued_draw->tag = tag;
        // FEAT-SGT-079 step 12: dispatches stay with the recorder (39% of B's runs held one).
        queued_draw->to_b = !compute && (push_pass / 4) % 2 == 1;
#ifdef RYTY_GPU
        // FEAT-SGT-079 step 17 (SHADGT_RECORDER_B_LEARNED=1; 2: in arm 1 of the 2 s A/B): B gets
        // the passes whose output waited 0.5 ms+ in one of the last two frames, matched by key and
        // occurrence (GT7's double-buffered targets alternate keys from frame to frame).
        if (pass_start) {
            push_draw_in_pass = 0;
        }
        if (const u32 chunk = RecorderBChunk(); chunk != 0) { // FEAT-SGT-079 step 18
            // SHADGT_RECORDER_B_CHUNK_AB=1: B takes no chunk in arm 0 of the 2 s A/B.
            static const bool chunk_ab = [] {
                const char* env = std::getenv("SHADGT_RECORDER_B_CHUNK_AB");
                return env && env[0] == '1';
            }();
            queued_draw->to_b = !compute && (push_draw_in_pass / chunk) % 2 == 1 &&
                                !(chunk_ab &&
                                  ::Vulkan::SubmitAbArm().load(std::memory_order_relaxed) == 0);
        }
        ++push_draw_in_pass;
        if (pass_start) {
            push_occurrence_hash =
                Vulkan::NativeRenderer::PassOccurrenceHash(tag, push_occurrences[tag]++);
        }
        if (RecorderBLearned()) {
            auto* native = rasterizer ? rasterizer->Native() : nullptr;
            queued_draw->to_b = !compute && native && native->LearnedWantsB(push_occurrence_hash);
        }
#endif
        // FEAT-SGT-079 step 13 (SHADGT_RECORDER_B_FIRST_A=1; 2: in arm 1 of the 2 s A/B): the
        // first draw of each of B's passes is the recorder's. A pass brings the pictures it
        // samples up to date at its first draw (rewrites, sources: 37% of B's passes, against
        // 2% later in the pass), which is ordered work B cannot do ahead.
        if (pass_start && queued_draw->to_b && RecorderBFirstToA()) {
            queued_draw->to_b = false;
        }
        queued_draw->pushed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();
        if (b_thread.joinable()) {
            bool was_empty;
            {
                std::scoped_lock lk{b_mutex};
                was_empty = b_queue.empty();
                b_queue.push_back(queued_draw);
            }
            if (was_empty) b_wake.notify_one();
        }
    }
    const std::vector<u32>& job_delta = queued_draw && delta.empty() ? queued_draw->delta : delta;
    const void* const delta_hint = job_delta.data();
    const size_t delta_hint_bytes = std::min<size_t>(job_delta.size() * sizeof(u32), 512);
    DrawPipe::Work job{[this, delta = std::move(delta), cs, cb_extent = last_cb_extent,
                     db_extent = last_db_extent, expected = std::move(expected),
                     selected = std::move(selected), draw = std::move(draw),
                     decode_epoch, capture = std::move(capture), staged, indirect_selected, ahead_sequence,
                     slot, queued_draw
#ifndef NDEBUG
                     , trace_id, pair_token
#endif
                     ]() mutable {
        RecorderParts::Scope part{RecorderParts::Part::DrawJobs}; // DIAG-GT7-005
        if (ahead_sequence && read_ahead_states) read_ahead_states->NoteRecorderStarted(ahead_sequence);
#ifndef NDEBUG
        GeometryTrace::Scope trace_scope{trace_id};
        ReadAheadPair::Scope pair_scope{pair_token, true};
        if (pair_token) ReadAheadPair::NoteRecorderStarted(pair_token);
        if (trace_id) GeometryTrace::Record("recorder_begin", trace_id);
#endif
        if (queued_draw && !b_thread.joinable()) { // FEAT-SGT-079: B's registers keep up
            if (RecorderBOverlap() && !::Vulkan::OnRecorderBThread()) {
                overlap_since_b.push_back(queued_draw); // step 19: B applies them before its job
            } else {
                RecorderBApply(*queued_draw);
            }
        }
        running_queued_draw = queued_draw.get();
        if (slot) { // PERF-SGT-077: what the select stage chose
            selected = std::move(slot->selected);
            indirect_selected = slot->indirect_selected;
        }
        auto& sel = staged ? staged->selected : selected;
#ifndef NDEBUG
        if (trace_id && sel) GeometryTrace::Record("selected", trace_id,
            reinterpret_cast<std::uintptr_t>(sel->pipeline),
            reinterpret_cast<std::uintptr_t>(sel->compute), indirect_selected, bool(staged));
#endif
        // FEAT-SGT-079 step 15: on B's thread, B's registers only (the recorder applies the
        // delta to its own when the job comes back to it).
        auto& state = b_state && recorder_state == b_state.get() ? *b_state : *recorder;
        RegsDelta<Regs::NumRegs>::Apply(queued_draw && delta.empty() ? queued_draw->delta : delta,
                                        std::span<u32, Regs::NumRegs>{state.regs.reg_array});
        if (cs) {
            state.cs = *cs;
        }
        state.cb_extent = cb_extent;
        state.db_extent = db_extent;
        if (expected && &state == recorder.get()) { // not on B's thread (FEAT-SGT-079)
            VerifyRecorderRegs(*expected);
        }
        if constexpr (Common::DiagnosticsBuilt) { // DIAG-SGT-085: the recorder ran a job of its own
            if (!b_state || recorder_state != b_state.get()) b_overlap_a_ran = true;
        }
        rasterizer->SetSelectedPipeline(sel.get());
        rasterizer->SetDrawEpoch(sel && !indirect_selected ? decode_epoch : 0);
        rasterizer->SetReadCapture(capture.get());
        rasterizer->SetBufferPlan(staged ? &staged->plan : nullptr);
        draw();
#ifndef NDEBUG
        if (trace_id) GeometryTrace::Record("recorder_end", trace_id);
#endif
        rasterizer->SetBufferPlan(nullptr);
        rasterizer->SetReadCapture(nullptr);
        rasterizer->SetDrawEpoch(0);
        rasterizer->SetSelectedPipeline(nullptr);
        running_queued_draw = nullptr;
        delta_recycler.Give(std::move(delta));
        if (sel) {
            rasterizer->RecycleSelectedPipeline(std::move(sel));
        }
    }};
    job.SetPrefetchHint(0, selection_hint, selection_hint ? 960 : 0);
    job.SetPrefetchHint(1, delta_hint, delta_hint_bytes);
#ifdef RYTY_GPU
    if (prepared_hint && Vulkan::NativeRenderer::PreparedPrefetchOn()) { // PERF-SGT-081
        job.SetPrefetchHint(2, prepared_hint, Vulkan::NativeRenderer::PreparedPrefetchBytes());
    }
#endif
    job.SetInfo(queued_draw.get()); // FEAT-SGT-079
    // FEAT-SGT-079: the job's pass, from the targets it draws into (a dispatch: 1), for a
    // recorder looking ahead for where passes start.
    if (compute) {
        job.SetTag(1);
    } else {
        u64 pass_key = XXH3_64bits(&regs.color_buffers, sizeof(regs.color_buffers));
        pass_key = XXH3_64bits_withSeed(&regs.depth_buffer, sizeof(regs.depth_buffer), pass_key);
        job.SetTag(pass_key | 2);
    }
    if (queued_draw && queued_draw->to_b && baton_thread.joinable()) {
        // FEAT-SGT-079 step 5: B's jobs run on recorder B's thread while the recorder waits.
        const u64 tag = job.Tag();
        const void* const info = job.Info();
        job = DrawPipe::Work{[this, inner = std::move(job), qd = queued_draw]() mutable {
            if (RecorderBMode() == 6 && ::Vulkan::SubmitAbArm().load(std::memory_order_relaxed) == 0) {
                inner();
                return;
            }
            if (RecorderBOverlap()) { // FEAT-SGT-079 step 19
                const auto cost_now = [] {
                    return std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                        .count();
                };
                s64 cost_from = 0;
                if constexpr (Common::DiagnosticsBuilt) cost_from = cost_now();
                // B's jobs back to back (nothing of the recorder's between them) just queue; else
                // B's last list goes in first, then the recorder's own.
                const bool chunk_start =
                    !overlap_outstanding || !overlap_since_b.empty() || overlap_pass_tag != qd->tag;
                if (chunk_start) {
                    RecorderBSync();
                }
                s64 prepare_from = 0;
                if constexpr (Common::DiagnosticsBuilt) prepare_from = cost_now();
#ifdef RYTY_GPU
                auto* native = rasterizer->Native();
                if (native && chunk_start) {
                    // Step 21: this draw and B's next ones of its chunk (queued behind it) are
                    // readied under one lock; their hand-overs then just queue.
                    static thread_local std::vector<std::pair<u64, const QueuedDraw*>> ahead;
                    PeekQueuedDraws(ahead, 128);
                    static thread_local std::vector<QueuedDraw*> chunk;
                    static thread_local std::vector<const void*> packets;
                    static thread_local std::vector<u8> results;
                    chunk.assign(1, qd.get());
                    for (const auto& [ahead_tag, draw] : ahead) {
                        if (!draw || !draw->to_b || draw->tag != qd->tag || chunk.size() >= 64) break;
                        chunk.push_back(const_cast<QueuedDraw*>(draw));
                    }
                    packets.clear();
                    for (const auto* d : chunk) packets.push_back(d->packet);
                    results.assign(chunk.size(), 0);
                    native->RecorderBPrepareChunk(packets, results);
                    for (size_t i = 0; i < chunk.size(); ++i) chunk[i]->overlap_ready = results[i];
                }
                // Step 20: the draw's pictures ready first, in the recorder's list (spliced now,
                // ahead of B's); a draw that cannot be readied stays the recorder's, after B's.
                const bool keep =
                    native && (qd->overlap_ready == 2 ||
                               (qd->overlap_ready == 0 && !native->RecorderBPrepareForB(qd->packet)));
                if (keep) {
                    RecorderBSync();
                    if constexpr (Common::DiagnosticsBuilt) ++overlap_cost.kept;
                    qd->to_b = false;
                    inner();
                    return;
                }
#endif
                s64 handoff_from = 0;
                if constexpr (Common::DiagnosticsBuilt) {
                    handoff_from = cost_now();
                    overlap_cost.prepare += handoff_from - prepare_from;
                }
                {
                    std::scoped_lock lk{overlap_mutex};
                    overlap_queue.push_back({std::move(inner), std::move(overlap_since_b), qd});
                    ++overlap_pushed;
                    if (RecorderBOverlap() == 1) overlap_go = overlap_pushed;
                }
                overlap_since_b.clear();
                overlap_cv.notify_one();
                overlap_outstanding = true;
                overlap_pass_tag = qd->tag;
#ifdef RYTY_GPU
                if (auto* native = rasterizer->Native()) native->SetRecorderBOutstanding(true);
#endif
                RegsDelta<Regs::NumRegs>::Apply(
                    qd->delta, std::span<u32, Regs::NumRegs>{recorder->regs.reg_array});
                if (qd->cs) recorder->cs = *qd->cs;
                recorder->cb_extent = qd->cb_extent;
                recorder->db_extent = qd->db_extent;
                if constexpr (Common::DiagnosticsBuilt) {
                    overlap_cost.handoff += cost_now() - handoff_from;
                    ++overlap_cost.jobs;
                    ReportOverlapCost();
                    static_cast<void>(cost_from);
                }
                return;
            }
            auto run = [&inner] { inner(); };
#ifdef RYTY_GPU
            if (auto* native = rasterizer->Native()) {
                // FEAT-SGT-079 step 14: a draw that needs ordered work stays the recorder's.
                const bool hoist = RecorderBHoist();
                if ((hoist || RecorderBFirstToA()) && !native->RecorderBCanTake(qd->packet, hoist)) {
                    qd->to_b = false;
                    inner();
                    return;
                }
                native->RecorderBEnter(); // FEAT-SGT-079
            }
#endif
            s64 b_job_from = 0;
            if constexpr (Common::DiagnosticsBuilt) b_job_from = OverlapNowNs(); // DIAG-SGT-085
            RunOnRecorderB(run);
            if constexpr (Common::DiagnosticsBuilt) NoteOverlap(b_job_from, OverlapNowNs());
            // FEAT-SGT-079 step 15: the recorder's registers take the job's delta too, in order.
            RegsDelta<Regs::NumRegs>::Apply(qd->delta,
                                            std::span<u32, Regs::NumRegs>{recorder->regs.reg_array});
            if (qd->cs) recorder->cs = *qd->cs;
            recorder->cb_extent = qd->cb_extent;
            recorder->db_extent = qd->db_extent;
            if constexpr (Common::DiagnosticsBuilt) { // DIAG-SGT-083: B's registers are A's
                static u32 checked = 0;
                if (b_state && (++checked & 255) == 0) {
                    const auto& a_regs = recorder->regs.reg_array;
                    const auto& b_regs = b_state->regs.reg_array;
                    if (a_regs != b_regs) {
                        static std::atomic<u32> logged{0};
                        u32 first = 0;
                        while (first < Regs::NumRegs && a_regs[first] == b_regs[first]) ++first;
                        if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
                            LOG_WARNING(Render, "DIAG-SGT-083: recorder B's registers differ "
                                        "from the recorder's (first at {:#x}: {:#x} against {:#x})",
                                        first, b_regs[first], a_regs[first]);
                        }
                    } else if (checked == 256 * 64) {
                        LOG_INFO(Render, "DIAG-SGT-083: recorder B's registers match the "
                                 "recorder's after B's jobs (64 checks)");
                    }
                }
            }
        }};
        job.SetTag(tag);
        job.SetInfo(info);
    }
    if (defer_select) {
        draw_pipe->PushSelected(std::move(select_work), std::move(job), &slot->hint); // PERF-SGT-077
    } else {
        draw_pipe->Push(std::move(job), kind, std::move(prepare), std::move(order));
    }
    if (read_ahead_states && !compute && !ReadAheadSkipped()) {
        OfferDrawState(ahead_sequence
#ifndef NDEBUG
            , ReadAheadPair::Metadata{pair_token, indirect != nullptr}
#endif
        );
    }
    ReportDrawPipe();
    ReportEarlyFences();
}

// PERF-037: the registers the draw packets write, left out of the read-ahead state hash.
#define REG_DWORD(field) (offsetof(Regs, field) / sizeof(u32))
static_assert(REG_DWORD(index_base_address) == ReadAheadStates::PerDrawRegisters[0] &&
              REG_DWORD(draw_initiator) == ReadAheadStates::PerDrawRegisters[2] &&
              REG_DWORD(max_index_size) == ReadAheadStates::PerDrawRegisters[3] &&
              REG_DWORD(num_indices) == ReadAheadStates::PerDrawRegisters[4] &&
              sizeof(Regs::index_base_address) == 2 * sizeof(u32));
// FIX-220: DB_ALPHA_TO_MASK is context register 0x2DC, DB_DEPTH_CONTROL 0x200.
static_assert(REG_DWORD(db_alpha_to_mask) - REG_DWORD(depth_control) == 0x2DC - 0x200);
#undef REG_DWORD

// FIX-014: draws the rasterizer filters out (Rasterizer::FilterDraw) never get a pipeline.
static bool FilteredDraw(const Regs& state) {
    using OperationMode = ColorControl::OperationMode;
    const auto mode = state.color_control.mode;
    return state.primitive_type == PrimitiveType::None ||
           mode == OperationMode::EliminateFastClear || mode == OperationMode::FmaskDecompress ||
           mode == OperationMode::Resolve;
}

void Liverpool::OfferDrawState(u64 sequence
#ifndef NDEBUG
    , ReadAheadPair::Metadata metadata
#endif
) {
    // The draw being decoded is still ahead of the recorder. Its state is hashed only when a
    // register its pipeline key comes from was written since the previous draw.
    if (pipeline_regs_written && read_ahead_states->Space() > 0) {
        pipeline_regs_written = false;
        if (!FilteredDraw(regs)) {
            read_ahead_states->Offer(regs.reg_array, sequence
#ifndef NDEBUG
                                     , metadata
#endif
                                     );
        }
    }
    // A pipeline miss on the recorder: new pipelines are appearing, so the draws after this
    // one are read now, not only from the next command buffer on.
    // A read-ahead that stopped on a full queue goes on once a quarter of it is free again.
    if (read_ahead_states->TakeNewMiss() ||
        (read_ahead_stopped_early && read_ahead_states->Space() >= 128)) {
        PipeReadAhead();
    }
}

void Liverpool::PipeReadAhead() {
    const size_t space = read_ahead_states->Space();
    if (space == 0) {
        read_ahead_stopped_early = true; // Again at the next draw.
        return;
    }
#ifndef NDEBUG
    const auto start = std::chrono::steady_clock::now();
#endif
    read_ahead_stopped_early = ScanAheadForPipelines(
        [&](const Regs& state) {
            return !FilteredDraw(state) && read_ahead_states->Offer(state.reg_array);
        },
        4096, static_cast<u32>(std::min<size_t>(space, 256)));
#ifndef NDEBUG
    ++pipe_read_ahead_stats.scans;
    pipe_read_ahead_stats.ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
#endif
}

// PERF-GT7-046: a queue's wait for a label no queued work writes stopped the command thread until
// the recorder had finished all it was given, ~4 ms a frame on GT7's pre-race screens, and nothing
// else decoded meanwhile. The queue now yields until the recorder has run dry, and only then asks
// for it. SHADGT_NATIVE_WAIT_NO_DRAIN=0: the wait drains the recorder at once.
// PERF-SGT-078 (SHADGT_WAIT_FIRST_DRAIN=0; =2 in turns every 2 s; 1 or unset: as before): a compute
// queue's WAIT_REG_MEM that its label does not meet yet checks a queued label write and the pending
// fences without draining the recorder first. GT7 race (Release, nsys): that first drain was 7.2% of
// the command processor's time, which decodes nothing while it waits for the recorder to catch up,
// and decoding is what limits GT7's frame with keep-written mirrors (PERF-GT7-193). The checks run
// in the order a label write moves through: queued (pending_labels, which the recorder clears only
// after it registered the fence), pending fence (removed only after its value is in memory), memory.
// A check that misses one keeps waiting, the loop drains when a label is written over the wait.
static bool WaitSkipsFirstDrain() {
    static const int mode = [] {
        const char* env = std::getenv("SHADGT_WAIT_FIRST_DRAIN");
        return env && (env[0] == '0' || env[0] == '2') ? (env[0] == '0' ? 1 : 2) : 0;
    }();
    if (mode != 2) {
        return mode == 1;
    }
    return ::Vulkan::SubmitAbArm().load(std::memory_order_relaxed) == 1;
}

bool Liverpool::WaitsYieldToRecorder() const {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADGT_NATIVE_WAIT_NO_DRAIN");
        return !(env && env[0] == '0');
    }();
    return enabled && draw_pipe;
}

void Liverpool::SyncRecorder(std::string_view reason) {
    if (!draw_pipe) {
        return;
    }
    Common::Nvtx::Scope nvtx{"drain recorder"}; // DIAG-056
#ifndef NDEBUG
    const auto start = std::chrono::steady_clock::now();
#endif
    draw_pipe->Drain();
#ifndef NDEBUG
    auto& entry = sync_reasons[reason];
    ++entry.first;
    entry.second +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
#endif
}

void Liverpool::RecordLabelWrite(VAddr address, std::vector<u8> value,
                                 Common::UniqueFunction<void>&& work) {
    if (waiting_queues != 0) { // PERF-SGT-028: a write over a label a compute queue waits on
        for (size_t slot = 0; slot < waited_labels.size(); ++slot) {
            const VAddr waited = waited_labels[slot];
            if (waited != 0 && address <= waited && waited < address + value.size()) {
                ++label_writes_over_wait[slot];
            }
        }
    }
    if (!draw_pipe || value.empty() || address == 0) {
        Record(std::move(work));
        return;
    }
    const u64 job = ++label_jobs;
    if (early_fences_mode) {
        // PERF-063: a fence written by the recorder; the next window starts after it.
#ifndef NDEBUG
        ++early_stats.normal;
#endif
        fences_decoded.fetch_add(1, std::memory_order_relaxed);
        NewWindow();
    }
    if (waited_label != 0 && address <= waited_label && waited_label < address + value.size()) {
        ++labels_written_while_waiting; // DIAG-047
    }
    const u64 label_size = value.size();
    {
        std::scoped_lock lk{pending_labels_mutex};
        pending_labels[address] = PendingLabel{std::move(value), job};
    }
    // PERF-082: with the buffer stage, a label write does not stop it: the recorder redoes a
    // planned buffer the label lands in. -DisablePerf 95 keeps label writes as barriers.
    static const bool labels_neutral = Common::PerfFeatureEnabled(95);
    const bool neutral = labels_neutral && draw_pipe->HasStage();
    u64 label_seq = 0;
    if (neutral) {
        label_seq = labels_pushed.load(std::memory_order_relaxed) + 1;
        auto& record = label_ring[label_seq % label_ring.size()];
        record.address = address;
        record.size = label_size;
        record.seq.store(label_seq, std::memory_order_release);
        labels_pushed.store(label_seq, std::memory_order_release);
    }
    DrawPipe::SetPushTag("label write"); // DIAG-069
    draw_pipe->Push([this, address, job, label_seq, work = std::move(work)] {
        RecorderParts::Scope part{RecorderParts::Part::Labels}; // DIAG-GT7-005
        work();
        if (label_seq != 0) {
            labels_done.store(label_seq, std::memory_order_release);
        }
        if (early_fences_mode) {
            fences_delivered.fetch_add(1, std::memory_order_release);
        }
        std::scoped_lock lk{pending_labels_mutex};
        if (const auto it = pending_labels.find(address);
            it != pending_labels.end() && it->second.job == job) {
            pending_labels.erase(it);
        }
    }, neutral ? DrawPipe::JobKind::Neutral : DrawPipe::JobKind::Barrier);
}

bool Liverpool::LabelWrittenOver(u64 done, u64 pushed, VAddr address, u64 size) const {
    if (pushed <= done) {
        return false;
    }
    if (pushed - done >= label_ring.size()) {
        return true; // Too many to look at: assume one did.
    }
    for (u64 seq = done + 1; seq <= pushed; ++seq) {
        const auto& record = label_ring[seq % label_ring.size()];
        if (record.seq.load(std::memory_order_acquire) != seq) {
            return true; // Overwritten meanwhile.
        }
        if (record.address < address + size && address < record.address + record.size) {
            return true;
        }
    }
    return false;
}

bool Liverpool::PendingLabelSatisfies(const PM4CmdWaitRegMem& wait) {
    if (!draw_pipe || wait.mem_space != PM4CmdWaitRegMem::MemSpace::Memory) {
        return false;
    }
    const auto address = reinterpret_cast<VAddr>(wait.Address<u32*>());
    std::scoped_lock lk{pending_labels_mutex};
    for (const VAddr start : {address, address - sizeof(u32)}) {
        const auto it = pending_labels.find(start);
        if (it == pending_labels.end() || address + sizeof(u32) > start + it->second.bytes.size()) {
            continue;
        }
        u32 value;
        std::memcpy(&value, it->second.bytes.data() + (address - start), sizeof(u32));
        return wait.TestValue(value);
    }
    return false;
}

void Liverpool::RecorderWaitRegMem(const PM4CmdWaitRegMem& wait) {
    // PERF-063: a CPU fault does not wait for the recorder while it may wait for that CPU.
    recorder_in_wait.store(true, std::memory_order_release);
    SCOPE_EXIT {
        recorder_in_wait.store(false, std::memory_order_release);
    };
    // The job that writes the label ran before this one. If its fence was deferred, the value
    // reaches memory when the GPU finishes, written by the scheduler's completion thread.
    const auto& state_regs = recorder->regs.reg_array;
    if (wait.Test(state_regs) || SatisfiedByPendingFence(rasterizer, &wait)) {
        return;
    }
#ifndef NDEBUG
    const auto start = std::chrono::steady_clock::now();
#endif
    rasterizer->FlushForDeferredFences();
    while (!wait.Test(state_regs) && !SatisfiedByPendingFence(rasterizer, &wait)) {
        draw_pipe->RunUrgent(); // PERF-050: no job is half done here
        std::this_thread::yield();
    }
    VideoCore::BumpUploadEpoch();
#ifndef NDEBUG
    static u32 reports = 0;
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (ms > 100.0 && reports++ < 10) {
        LOG_WARNING(Render, "PERF-031: the recorder waited {:.0f} ms for label {:#x}", ms,
                    reinterpret_cast<uintptr_t>(wait.Address<u32*>()));
    }
#endif
}

void Liverpool::VerifyRecorderRegs(const Regs& expected) {
    // SHADGT_DRAW_PIPE_VERIFY=N: a register write the command thread did not mark would leave
    // the recorder's copy stale; name the first dword that differs.
    static u32 reports = 0;
    const auto& actual = recorder->regs.reg_array;
    for (u32 i = 0; i < Regs::NumRegs; ++i) {
        if (actual[i] != expected.reg_array[i]) {
            if (reports++ < 20) {
                LOG_ERROR(Render, "PERF-031: recorder register {:#x} is {:#x}, expected {:#x}", i,
                          actual[i], expected.reg_array[i]);
            }
            recorder->regs = expected;
            return;
        }
    }
}

void Liverpool::ReportDrawPipe() {
#ifndef NDEBUG
    const auto now = std::chrono::steady_clock::now();
    if (now - pipe_report_start < std::chrono::seconds{2}) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - pipe_report_start).count();
    pipe_report_start = now;
    const auto stats = draw_pipe->TakeStats();
    std::string reasons;
    for (const auto& [reason, entry] : sync_reasons) {
        reasons += fmt::format(" {}={}/{:.1f}ms", reason, entry.first, entry.second);
    }
    sync_reasons.clear();
    std::string read_ahead;
    if (read_ahead_states) {
        read_ahead = fmt::format("; read-ahead: {} draw states queued, {} reads ahead ({:.1f} ms), "
                                 "{} stopped at unpublished REWINDs",
                                 read_ahead_states->TakeOffered(), pipe_read_ahead_stats.scans,
                                 pipe_read_ahead_stats.ms, pipe_read_ahead_stats.rewinds);
        pipe_read_ahead_stats = {};
    }
    if (commands_recorded != 0) {
        read_ahead += fmt::format("; {} commands run on the recorder", commands_recorded);
        commands_recorded = 0;
    }
    if (labels_written_while_waiting != 0) {
        read_ahead += fmt::format("; {} waited-on labels written by queued jobs during the wait",
                                  labels_written_while_waiting);
        labels_written_while_waiting = 0;
    }
    if (const u64 checked = diag_moved_waits_checked.exchange(0); checked != 0) {
        read_ahead += fmt::format("; DIAG-054: {} moved waits checked, {} with changed commands",
                                  checked, diag_moved_waits_changed.exchange(0));
    }
    if (waits_moved_after_turns != 0) {
        read_ahead += fmt::format("; {} waits moved after the other queues decoded",
                                  waits_moved_after_turns);
        waits_moved_after_turns = 0;
    }
    {
        std::scoped_lock lk{submit_timing_mutex};
        const auto& t = submit_stats;
        if (t.count != 0) {
            read_ahead += fmt::format(
                "; DIAG-051: {} gfx submits, after the game's submit: decoded {:.2f} ms, recorded "
                "{:.2f} ms, GPU done {:.2f} ms (max {:.1f}); game submits every {:.2f} ms; "
                "DIAG-055: submits come {:.2f} ms after the last EOP interrupt; EOPs {} deferred, "
                "{} at once, {} EXP-063 early fences",
                t.count, t.decode_us / 1000.0 / t.count, t.record_us / 1000.0 / t.count,
                t.gpu_count ? t.gpu_us / 1000.0 / t.gpu_count : 0.0, t.max_gpu_us / 1000.0,
                t.gaps ? t.gap_us / 1000.0 / t.gaps : 0.0,
                t.eop_to_submit_count ? t.eop_to_submit_us / 1000.0 / t.eop_to_submit_count
                                      : 0.0,
                g_eops_deferred.exchange(0), g_eops_immediate.exchange(0),
                g_fences_early.exchange(0));
        }
        submit_stats = {};
    }
    if (const u64 flushes = fault_flushes.exchange(0); flushes != 0) {
        const u64 wait_us = fault_flush_wait_us.exchange(0);
        read_ahead += fmt::format(
            "; DIAG-050: {} CPU fault flushes, game threads waited {:.1f} ms (avg {:.2f}, max "
            "{:.1f})",
            flushes, wait_us / 1000.0, wait_us / 1000.0 / flushes,
            fault_flush_max_us.exchange(0) / 1000.0);
    }
    LOG_WARNING(Render,
                "PERF-031 draw pipe in {:.1f} s: {} jobs, recorder busy {:.0f}%, max queued {}, "
                "{} full-queue waits; {} waits moved to the recorder; drains {} ({} waited, "
                "{:.1f} ms):{}{}",
                seconds, stats.pushed, stats.recorder_busy_us / (seconds * 1e4), stats.max_queued,
                stats.push_waits, waits_moved, stats.drains, stats.drains_that_waited,
                stats.drain_wait_us / 1000.0, reasons, read_ahead);
    if (draw_pipe->HasStage()) {
        LOG_WARNING(Render,
                    "PERF-067 buffer stage in {:.1f} s: {} draws prepared ahead, {} waits for the "
                    "recorder to catch up ({} at barrier jobs); DIAG-084 stage time: idle {} ms, "
                    "preparing {} ms, held back by the lead limit {} ms, waiting after barriers "
                    "{} ms",
                    seconds, stats.stage_prepared, stats.stage_syncs, stats.stage_barriers,
                    stats.stage_idle_us / 1000, stats.stage_prepare_us / 1000,
                    stats.stage_lead_us / 1000, stats.stage_sync_us / 1000);
    }
    for (size_t q = 0; q < wait_drains.size(); ++q) { // DIAG-SGT-027
        const auto& t = wait_drains[q];
        if (t.packets == 0) continue;
        LOG_WARNING(Render,
                    "DIAG-SGT-027 {} WAIT_REG_MEM drains in {:.1f} s: {} waits drained, ended "
                    "after the first drain by the label {} / by a recorded fence {}, {} looped; "
                    "{} loop turns ({:.1f} a looping wait), {} of them drained ({} as a 64th-turn "
                    "recheck); looping waits ended by a recorded fence {} / by the label {}; "
                    "{:.1f} ms until the recorder ran dry before the first drain",
                    q == 0 ? "gfx" : "asc", seconds, t.packets, t.label_first, t.fence_first,
                    t.loops, t.turns, t.loops ? double(t.turns) / double(t.loops) : 0.0,
                    t.loop_drains, t.rechecks, t.loop_end_fence, t.loop_end_label,
                    t.idle_wait_us / 1000.0);
    }
    wait_drains = {};
    waits_moved = 0;
#endif
}

bool Liverpool::WritesLiveCommands(VAddr address, u64 size) const {
    for (const auto& span : live_cmd_buffers) {
        const auto begin = reinterpret_cast<VAddr>(span.data());
        const auto end = begin + span.size_bytes();
        if (address < end && address + size > begin) {
            return true;
        }
    }
    return false;
}

int Liverpool::RecorderBMode() {
    static const int mode = [] {
        const char* env = std::getenv("SHADGT_RECORDER_B");
        return env && env[0] >= '1' && env[0] <= '8' ? env[0] - '0' : 0;
    }();
    return mode;
}

s64 Liverpool::OverlapNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// DIAG-SGT-085 (diagnostics builds, SHADGT_DIAG_EOP_LATENCY=1): under the baton, how much A and B
// could overlap. A run of B's (its jobs between two jobs of the recorder's) could run beside the
// recorder's work that follows it, up to the next run of B's: the smaller of the two is what
// overlapping that pair would save at most. The recorder's segment is wall time (it includes the
// recorder waiting for work), so the sum is an upper bound.
void Liverpool::NoteOverlap(s64 from, s64 to) {
    auto& o = b_overlap;
    if (o.since == 0) o.since = from;
    if (b_overlap_a_ran) { // a segment of the recorder's ended: the last run of B's pairs with it
        b_overlap_a_ran = false;
        if (o.last_end != 0) {
            const s64 a_seg = from - o.last_end;
            o.b += o.run;
            o.a += a_seg;
            o.save += std::min(o.run, a_seg);
            ++o.pairs;
#ifdef RYTY_GPU
            if (auto* native = rasterizer->Native()) { // DIAG-SGT-086: up to A needing B's run
                const s64 free = std::min(native->RecorderBTakeFreeNs(from), a_seg);
                o.free += free;
                o.save_free += std::min(o.run, free);
            }
#endif
        }
        o.run = 0;
    }
    o.run += to - from;
    o.last_end = to;
    if (to - o.since >= 5'000'000'000 && o.pairs != 0) {
        const double secs = double(to - o.since) / 1e9;
        LOG_INFO(Render, "DIAG-SGT-085: overlap bound in {:.1f} s: {} pairs; B runs {:.1f} ms/s, the "
                 "recorder's segments after them {:.1f} ms/s, the smaller of each pair summed "
                 "{:.1f} ms/s; DIAG-SGT-086: the segments up to needing B's run {:.1f} ms/s, the "
                 "smaller summed {:.1f} ms/s",
                 secs, o.pairs, double(o.b) / 1e6 / secs, double(o.a) / 1e6 / secs,
                 double(o.save) / 1e6 / secs, double(o.free) / 1e6 / secs,
                 double(o.save_free) / 1e6 / secs);
        o.b = o.a = o.save = o.free = o.save_free = 0;
        o.pairs = 0;
        o.since = to;
    }
}

// SHADGT_RECORDER_B_FIRST_A: 1 the first draw of each of B's passes is the recorder's (step 13);
// 2 that in arm 1 of the 2 s A/B; 3 B keeps it and the recorder does its ordered work at the
// hand-over (step 16, the hoist); 4 the hoist in arm 1 and step 13 in arm 0; 5 both (the first
// draw is the recorder's: its targets' rewrites; the others' work is hoisted); 6 5 in arm 1 and
// 1 in arm 0.
static int RecorderBFirstMode() {
    static const int mode = [] {
        const char* env = std::getenv("SHADGT_RECORDER_B_FIRST_A");
        return env && env[0] >= '1' && env[0] <= '6' ? env[0] - '0' : 0;
    }();
    return mode;
}

// DIAG-SGT-088: every 5 s, the overlap's costs on the recorder (ms a second) and B's wait for the
// renderer's lock: what the lock's shrinking can win back (the waits) and what it cannot (the
// readying, the hand-over, the splices).
void Liverpool::ReportOverlapCost() {
    auto& c = overlap_cost;
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (c.since == 0) c.since = now;
    if (now - c.since < 5'000'000'000) return;
    const double secs = double(now - c.since) / 1e9;
    u64 b_wait = 0;
    u64 a_wait = 0;                     // DIAG-SGT-089
    std::array<std::array<u64, 2>, 2> locked{};
    std::array<u64, 2> locked_draws{};
#ifdef RYTY_GPU
    if (auto* native = rasterizer ? rasterizer->Native() : nullptr) {
        b_wait = native->b_mutex_wait_ns.exchange(0, std::memory_order_relaxed);
        a_wait = native->a_mutex_wait_ns.exchange(0, std::memory_order_relaxed);
        for (u32 t = 0; t < 2; ++t) {
            locked_draws[t] = native->draw_locked_count[t].exchange(0, std::memory_order_relaxed);
            for (u32 p = 0; p < 2; ++p) {
                locked[t][p] = native->draw_locked_ns[t][p].exchange(0, std::memory_order_relaxed);
            }
        }
    }
#endif
    LOG_INFO(Render, "DIAG-SGT-088: overlap in {:.1f} s: {} jobs to B, {} kept, {} syncs; the "
             "recorder waiting for B {:.1f} ms/s, readying B's draws {:.1f} ms/s, handing over "
             "{:.1f} ms/s, splicing B's lists {:.1f} ms/s; B waiting for the renderer's lock "
             "{:.1f} ms/s",
             secs, c.jobs, c.kept, c.syncs, c.sync_wait / 1e6 / secs, c.prepare / 1e6 / secs,
             c.handoff / 1e6 / secs, c.commit / 1e6 / secs, b_wait / 1e6 / secs);
    const auto per_draw = [&](u32 t, u32 p) {
        return locked_draws[t] ? double(locked[t][p]) / 1e3 / double(locked_draws[t]) : 0.0;
    };
    LOG_INFO(Render, "DIAG-SGT-089: in Draw with the renderer's lock: the recorder {} draws, "
             "{:.1f} ms/s before the encode side + {:.1f} ms/s in it ({:.2f} + {:.2f} us a draw), "
             "waiting for the lock while B holds work {:.1f} ms/s; B {} draws, {:.1f} + {:.1f} "
             "ms/s ({:.2f} + {:.2f} us a draw)",
             locked_draws[0], locked[0][0] / 1e6 / secs, locked[0][1] / 1e6 / secs, per_draw(0, 0),
             per_draw(0, 1), a_wait / 1e6 / secs, locked_draws[1], locked[1][0] / 1e6 / secs,
             locked[1][1] / 1e6 / secs, per_draw(1, 0), per_draw(1, 1));
    c = {};
    c.since = now;
}

int Liverpool::RecorderBOverlap() {
    static const int mode = [] {
        const char* env = std::getenv("SHADGT_RECORDER_B_OVERLAP");
        return env && (env[0] == '1' || env[0] == '2') ? env[0] - '0' : 0;
    }();
    return mode;
}

// FEAT-SGT-079 step 19: recorder B's thread with a queue. It brings its registers up to each job
// (the recorder's draws since its last job, then the job's own), runs it, and counts it done. In
// late B (2) it waits for the recorder's go, so it records only when the recorder syncs.
void Liverpool::RecorderBOverlapLoop() {
    while (true) {
        OverlapJob job;
        {
            std::unique_lock lk{overlap_mutex};
            overlap_cv.wait(lk, [this] {
                return baton_quit.load(std::memory_order_acquire) ||
                       (!overlap_queue.empty() &&
                        overlap_go > overlap_done.load(std::memory_order_acquire));
            });
            if (baton_quit.load(std::memory_order_acquire) && overlap_queue.empty()) return;
            job = std::move(overlap_queue.front());
            overlap_queue.pop_front();
        }
        for (const auto& draw : job.catch_up) {
            RecorderBApply(*draw);
        }
        try {
            job.work();
        } catch (...) {
            std::scoped_lock lk{overlap_mutex};
            baton_error = std::current_exception();
        }
        overlap_done.fetch_add(1, std::memory_order_acq_rel);
        overlap_done.notify_all();
    }
}

// The recorder: B records everything it was handed, then its list goes into the command order
// (ahead of the recorder's open list, which is spliced later).
void Liverpool::RecorderBSync() {
    if (!overlap_outstanding) return;
    s64 cost_from = 0;
    if constexpr (Common::DiagnosticsBuilt) {
        cost_from = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    }
    u64 target;
    {
        std::scoped_lock lk{overlap_mutex};
        overlap_go = overlap_pushed;
        target = overlap_pushed;
    }
    overlap_cv.notify_one();
    for (u64 done = overlap_done.load(std::memory_order_acquire); done < target;
         done = overlap_done.load(std::memory_order_acquire)) {
        overlap_done.wait(done, std::memory_order_acquire);
    }
    if (baton_error) std::rethrow_exception(std::exchange(baton_error, nullptr));
    overlap_outstanding = false;
    s64 commit_from = 0;
    if constexpr (Common::DiagnosticsBuilt) {
        commit_from = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
        overlap_cost.sync_wait += commit_from - cost_from;
        ++overlap_cost.syncs;
    }
#ifdef RYTY_GPU
    if (auto* native = rasterizer ? rasterizer->Native() : nullptr) {
        native->SetRecorderBOutstanding(false);
        native->RecorderBCommit();
    }
#endif
    if constexpr (Common::DiagnosticsBuilt) {
        overlap_cost.commit += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count() -
                               commit_from;
    }
}

u32 Liverpool::RecorderBChunk() {
    static const u32 chunk = [] {
        const char* env = std::getenv("SHADGT_RECORDER_B_CHUNK");
        return env && *env ? u32(std::strtoul(env, nullptr, 10)) : 0u;
    }();
    return chunk;
}

bool Liverpool::RecorderBLearned() {
    static const int mode = [] {
        const char* env = std::getenv("SHADGT_RECORDER_B_LEARNED");
        return env && (env[0] == '1' || env[0] == '2') ? env[0] - '0' : 0;
    }();
    return mode == 1 || (mode == 2 && ::Vulkan::SubmitAbArm().load(std::memory_order_relaxed) == 1);
}

bool Liverpool::RecorderBFirstToA() {
    const int mode = RecorderBFirstMode();
    const int arm = ::Vulkan::SubmitAbArm().load(std::memory_order_relaxed);
    return mode == 1 || (mode == 2 && arm == 1) || (mode == 4 && arm == 0) || mode == 5 ||
           mode == 6;
}

bool Liverpool::RecorderBHoist() {
    const int mode = RecorderBFirstMode();
    const int arm = ::Vulkan::SubmitAbArm().load(std::memory_order_relaxed);
    return mode == 3 || mode == 5 || ((mode == 4 || mode == 6) && arm == 1);
}

void Liverpool::RecorderBPause() {
    _mm_pause();
}

// FEAT-SGT-079 step 5: recorder B's thread with the baton. It is set up as the recorder thread
// is (its thread-locals: the recorder's register state, the buffer cache's and the guest clock's
// marks) and counts as the recorder thread while it runs a job the recorder handed it.
void Liverpool::RecorderBBatonThread() {
    Common::SetCurrentThreadName("shadGT:GpuRecorderB");
    VideoCore::BufferCache::MarkRecorderThread();
    Common::GuestClock::MarkGpuCommandThread();
    // FEAT-SGT-079 step 11: B's own registers, brought up to each job in push order (every
    // job's delta is applied to them as it runs, RecorderBApply), not the recorder's.
    recorder_state = b_state ? b_state.get() : recorder.get();
    Common::PreferPerformanceCores(); // as the recorder (PERF-088)
#ifdef RYTY_GPU
    Common::NativeThreadPriority native_priority;
#endif
    draw_pipe->SetBatonThread(std::this_thread::get_id());
    ::Vulkan::MarkRecorderBThread(); // FEAT-SGT-079
    if (RecorderBOverlap()) {
        RecorderBOverlapLoop();
        return;
    }
    u64 seen = 0;
    while (true) {
#ifdef RYTY_GPU
        native_priority.Update(Vulkan::NativeModeRequested() && Common::NativeCpuPriorityRequested());
#endif
        u64 seq = baton_seq.load(std::memory_order_acquire);
        for (u32 spin = 0; seq == seen; ++spin) {
            if (spin < 16384) {
                RecorderBPause();
            } else {
                baton_seq.wait(seen, std::memory_order_acquire);
            }
            seq = baton_seq.load(std::memory_order_acquire);
        }
        seen = seq;
        if (baton_quit.load(std::memory_order_acquire)) {
            return;
        }
        try {
            baton_call(baton_ctx);
        } catch (...) {
            baton_error = std::current_exception();
        }
        baton_done.store(seq, std::memory_order_release);
        baton_done.notify_one();
    }
}

bool Liverpool::RecorderBOn() {
    return RecorderBMode() >= 3;
}

// FEAT-SGT-079 step 4 (SHADGT_RECORDER_B=4): recorder B's own thread. It takes every draw and
// dispatch in push order (its registers are its own: each delta applied once, here), and for
// the draws dealt to it does what B would do ahead of A: finds the draw's prepared draw and
// checks it against B's registers and the change logs. A still records everything; at each of
// B's draws it reads what B made of it, or that B had not got there yet.
void Liverpool::RecorderBThread(std::stop_token stop) {
    Common::SetCurrentThreadName("shadGT:GpuRecorderB");
    while (!stop.stop_requested()) {
        std::shared_ptr<QueuedDraw> draw;
        {
            std::unique_lock lk{b_mutex};
            if (!b_wake.wait(lk, stop, [this] { return !b_queue.empty(); })) {
                return;
            }
            draw = std::move(b_queue.front());
            b_queue.pop_front();
        }
        RecorderBApply(*draw);
        if (!draw->to_b || draw->compute) {
            continue;
        }
#ifdef RYTY_GPU
        if (auto* native = rasterizer->Native()) {
            native->RecorderBPrepare(draw.get(), b_state->regs);
        }
#endif
    }
}

void Liverpool::PeekQueuedDraws(std::vector<std::pair<u64, const QueuedDraw*>>& out, size_t max) {
    out.clear();
    if (!draw_pipe) return;
    static thread_local std::vector<std::pair<u64, const void*>> jobs;
    draw_pipe->PeekJobs(jobs, max);
    for (const auto& [tag, info] : jobs) {
        out.emplace_back(tag, static_cast<const QueuedDraw*>(info));
    }
}

void Liverpool::RecorderBApply(const QueuedDraw& draw) {
    if (!b_state || draw.seq <= b_applied_seq) return;
    RegsDelta<Regs::NumRegs>::Apply(draw.delta,
                                    std::span<u32, Regs::NumRegs>{b_state->regs.reg_array});
    if (draw.cs) b_state->cs = *draw.cs;
    b_state->cb_extent = draw.cb_extent;
    b_state->db_extent = draw.db_extent;
    b_applied_seq = draw.seq;
}

const Regs* Liverpool::RecorderBRegsAt(const QueuedDraw& draw) {
    if (!b_state || draw.seq < b_applied_seq) return nullptr;
    // Every draw before it must be applied first: the caller walks the queue in order.
    if (draw.seq != b_applied_seq + 1 && draw.seq != b_applied_seq) return nullptr;
    RecorderBApply(draw);
    return &b_state->regs;
}

void Liverpool::PeekQueuedTags(std::vector<u64>& out, size_t max) {
    out.clear();
    if (draw_pipe) draw_pipe->PeekTags(out, max);
}

bool Liverpool::IsGpuThread(std::thread::id id) const {
    return id == gpu_id || (Pipelined() && (id == draw_pipe->RecorderThreadId() ||
                                            id == draw_pipe->BatonThreadId())); // FEAT-SGT-079
}

#ifdef __linux__
bool Liverpool::IsGpuThreadTid(u32 tid) const {
    return tid == gpu_tid || (Pipelined() && tid == recorder_tid.load());
}
#endif

void Liverpool::OnGpuThreadFault() {
    if (Pipelined() && std::this_thread::get_id() == gpu_id) {
        draw_pipe->Drain();
    }
}

void Liverpool::SendFaultCommand(Common::UniqueFunction<void>&& func) {
    static const bool urgent = Common::PerfFeatureEnabled(62);
    Common::Nvtx::Scope nvtx{"CPU fault: wait for GPU data"}; // DIAG-056
#ifndef NDEBUG
    const auto start = std::chrono::steady_clock::now();
#endif
    if (draw_pipe && urgent) {
        std::binary_semaphore sem{0};
        draw_pipe->PushUrgent([&sem, &func] {
            func();
            sem.release();
        });
        sem.acquire();
    } else {
        SendCommand<true>([&func] { func(); });
    }
#ifndef NDEBUG
    const u64 us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
    fault_flushes.fetch_add(1, std::memory_order_relaxed);
    fault_flush_wait_us.fetch_add(us, std::memory_order_relaxed);
    u64 max = fault_flush_max_us.load(std::memory_order_relaxed);
    while (us > max && !fault_flush_max_us.compare_exchange_weak(max, us)) {
    }
#endif
}

bool Liverpool::CanSendFaultCommandAsync() const {
    static const bool urgent = Common::PerfFeatureEnabled(62);
    return draw_pipe != nullptr && urgent;
}

bool Liverpool::SendFaultCommandAsync(Common::UniqueFunction<void>&& func) {
    if (!CanSendFaultCommandAsync()) {
        return false;
    }
    draw_pipe->PushUrgent(std::move(func));
    return true;
}

bool Liverpool::CommandThreadFaultIsUrgent() {
    static const bool urgent = Common::PerfFeatureEnabled(62) && Common::PerfFeatureEnabled(65);
    return urgent && Pipelined() && std::this_thread::get_id() == gpu_id &&
           !draw_pipe->IsExclusive();
}

void Liverpool::ProcessCommands() {
    if (draw_pipe) {
        draw_pipe->RunUrgentIfExclusive(); // PERF-050
    }
    if (!num_commands) {
        return;
    }
    // PERF-031: commands from other threads (flips, cache flushes, unmaps) use the caches.
    // PERF-035: with the draw pipe they run on the recorder, in order behind the work decoded so
    // far (where a drain would have run them here), and decoding goes on meanwhile. A sender
    // that waits for its command still returns only once it ran. -DisablePerf 50 drains instead.
    static const bool commands_on_recorder = Common::PerfFeatureEnabled(50);
    const bool record = draw_pipe && commands_on_recorder;
    if (!record) {
        SyncRecorder("commands");
    }
    // Process incoming commands with high priority
    while (num_commands) {
        Common::UniqueFunction<void> callback{};
        {
            std::scoped_lock lk{submit_mutex};
            callback = std::move(command_queue.front());
            command_queue.pop();
            --num_commands;
        }
        if (record) {
            ++commands_recorded;
            // PERF-063: flips and fault flushes read no guest memory for the window's work (a
            // barrier for the buffer stage all the same).
            if (draw_pipe) {
                draw_pipe->Push(std::move(callback));
            } else {
                callback();
            }
        } else {
            callback();
        }
    }
}

void Liverpool::Process(std::stop_token stoken) {
    SCOPE_EXIT { Common::UpdateCoreOwner(0, false); };
#ifdef RYTY_GPU
    Common::NativeThreadPriority native_priority;
#endif
    Common::SetCurrentThreadName("shadGT:GpuCommandProcessor");
    Common::PreferPerformanceCores(); // PERF-088
    Common::GuestClock::MarkGpuCommandThread(); // FIX-044
    Common::SamplingProfiler::RegisterCurrentThread("GpuCommandProcessor"); // DIAG-048
    gpu_id = std::this_thread::get_id();
#ifdef __linux__
    gpu_tid = gettid();
#endif
    [[maybe_unused]] bool boosted = false;

    while (!stoken.stop_requested()) {
#ifdef RYTY_GPU
        native_priority.Update(Vulkan::NativeModeRequested() && Common::NativeCpuPriorityRequested());
#endif
        if (draw_pipe) {
            draw_pipe->ReleaseExclusive(); // PERF-050: idle; urgent work may run on the recorder
        }
        Common::UpdateCoreOwner(0, true); // PERF-GT7-048
        {
            std::unique_lock lk{submit_mutex};
            const auto has_work = [this] { return num_commands || num_tasks || submit_done; };
            // PERF-098: while the splash screen shows, or stored pipelines remain, the idle
            // command thread queues stored pipeline builds every few milliseconds.
            while (!has_work() && !stoken.stop_requested() && rasterizer &&
                   (Libraries::SystemService::IsSplashVisible() ||
                    Vulkan::GetPrecompileProgress().active)) {
                if (submit_cv.wait_for(lk, stoken, std::chrono::milliseconds{5}, has_work)) {
                    break;
                }
                lk.unlock();
                rasterizer->GetPipelineCache().IdlePrewarm(
                    Libraries::SystemService::IsSplashVisible());
                lk.lock();
            }
            const bool idle_diag = EopLatencyDiag() && !has_work(); // DIAG-SGT-080
            const s64 idle_from = idle_diag ? NowNs() : 0;
            const bool idle_open = idle_diag && g_last_submit_ns.load(std::memory_order_relaxed) >
                                                    g_last_decoded_ns.load(std::memory_order_relaxed);
            Common::CondvarWait(submit_cv, lk, stoken, has_work);
            if (idle_diag) {
                const s64 slept = NowNs() - idle_from;
                g_cp_idle_ns.fetch_add(slept, std::memory_order_relaxed);
                g_cp_idle_sleeps.fetch_add(1, std::memory_order_relaxed);
                if (idle_open) {
                    g_cp_idle_open_ns.fetch_add(slept, std::memory_order_relaxed);
                    g_cp_idle_open_sleeps.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (stoken.stop_requested()) {
            break;
        }
#ifdef RYTY_GPU
        // PERF-GT7-010: above normal priority while the native renderer asks for it, as the
        // recorder (the shader compiler's threads and the game's spinning jobs took cores).
        if (const bool boost = Vulkan::NativeRenderer::BoostOn(); boost != boosted) {
            boosted = boost;
            Common::SetCurrentThreadPriority(boost ? Common::ThreadPriority::High
                                                   : Common::ThreadPriority::Normal);
        }
#endif
        if (!draw_pipe && rasterizer && DrawPipeRequested()) {
            StartDrawPipe();
        }

        VideoCore::StartCapture();

        curr_qid = -1;

        while (num_tasks || num_commands) {
            ProcessCommands();

            curr_qid = (curr_qid + 1) % num_mapped_queues.load(std::memory_order_acquire);

            auto& queue = mapped_queues[curr_qid];

            Task::Handle task{};
            {
                std::scoped_lock lock{queue.m_access};
                if (queue.submits.empty()) {
                    continue;
                }
                task = queue.submits.front();
            }
            if (queue.nest.empty()) {
                task.resume();
            } else {
                // PERF-GTS-383: the innermost command buffer still being decoded is resumed
                // itself. One that finishes hands over to the buffer that called it, at once.
                while (true) {
                    Task::Handle inner = task;
                    for (size_t i = queue.nest.size(); i-- > 0;) {
                        if (!queue.nest[i].done()) {
                            inner = queue.nest[i];
                            break;
                        }
                    }
                    inner.resume();
                    if (inner == task || !inner.done()) {
                        break;
                    }
                }
            }

            if (task.done()) {
                task.destroy();

                {
                    std::scoped_lock lock{queue.m_access};
                    queue.submits.pop();
                    if (curr_qid != GfxQueueId) queue.pending.fetch_sub(1, std::memory_order_relaxed);
                }
                --num_tasks;
#ifndef NDEBUG
                // DIAG-051: a graphics submission is decoded; time it through the pipeline.
                std::optional<std::chrono::steady_clock::time_point> submitted;
                std::optional<SubmitRanges> ranges;
                if (curr_qid == GfxQueueId) {
                    std::scoped_lock lk{submit_timing_mutex};
                    if (!gfx_submit_times.empty()) {
                        submitted = gfx_submit_times.front();
                        gfx_submit_times.pop_front();
                    }
                    if (!gfx_submit_ranges.empty()) {
                        ranges = gfx_submit_ranges.front();
                        gfx_submit_ranges.pop_front();
                        Common::Nvtx::End(ranges->to_decoded);
                    }
                }
                const auto decoded = std::chrono::steady_clock::now();
#endif
                // PERF-031: the submission is finished once its recorded work is.
                RecordSafe([this
#ifndef NDEBUG
                    , submitted, decoded, ranges
#endif
                ] {
                    --num_submits;
#ifndef NDEBUG
                    u64 gpu_range = 0;
                    if (ranges) {
                        Common::Nvtx::End(ranges->to_recorded);
                        gpu_range = Common::Nvtx::Start(
                            fmt::format("gfx submit {}: recorded until GPU done", ranges->number));
                        if (!rasterizer) {
                            Common::Nvtx::End(gpu_range);
                        }
                    }
                    if (!submitted && rasterizer && gpu_range) {
                        rasterizer->WhenGpuDone([gpu_range] { Common::Nvtx::End(gpu_range); });
                    }
                    if (submitted && rasterizer) {
                        const auto recorded = std::chrono::steady_clock::now();
                        // DIAG-114 (SHADGT_DIAG_TIMELINE=1): 40 consecutive gfx submits after
                        // 200 s, times relative to the first one (ms): submit, decoded,
                        // recorded, GPU done, and the jobs pushed and finished by then.
                        static const bool timeline = [] {
                            const char* env = Common::DiagEnv("SHADGT_DIAG_TIMELINE");
                            return env && env[0] == '1';
                        }();
                        if (timeline) {
                            struct Row {
                                std::chrono::steady_clock::time_point submit, decoded, recorded;
                                std::atomic<s64> gpu_ns{};
                                u64 pushed, finished;
                            };
                            static std::array<Row, 40> rows;
                            static std::atomic<u32> next{};
                            static const auto start = std::chrono::steady_clock::now();
                            if (recorded - start > std::chrono::seconds{200} && next < rows.size()) {
                                const u32 i = next++;
                                auto& row = rows[i];
                                row.submit = *submitted;
                                row.decoded = decoded;
                                row.recorded = recorded;
                                row.pushed = draw_pipe ? draw_pipe->PushedJobs() : 0;
                                row.finished = draw_pipe ? draw_pipe->FinishedJobs() : 0;
                                rasterizer->WhenGpuDone([this, i] {
                                    rows[i].gpu_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch()).count();
                                    if (i + 1 == rows.size()) {
                                        std::string text;
                                        const auto t0 = rows[0].submit;
                                        const auto ms = [&](auto t) {
                                            return std::chrono::duration<double, std::milli>(t - t0).count();
                                        };
                                        for (u32 k = 0; k < rows.size(); ++k) {
                                            const auto& r = rows[k];
                                            const double gpu =
                                                (r.gpu_ns.load() - std::chrono::duration_cast<std::chrono::nanoseconds>(t0.time_since_epoch()).count()) / 1e6;
                                            text += fmt::format("\n  #{:2} submit {:8.2f} decoded {:8.2f} recorded {:8.2f} gpu {:8.2f} jobs {}",
                                                                k, ms(r.submit), ms(r.decoded), ms(r.recorded), gpu,
                                                                k ? r.finished - rows[k - 1].finished : 0);
                                        }
                                        text += "\n  recorder idle (ms):";
                                        for (const auto& idle : draw_pipe->RecentIdle()) {
                                            if (idle.from >= t0) {
                                                text += fmt::format(" {:.2f}-{:.2f}", ms(idle.from), ms(idle.to));
                                            }
                                        }
                                        LOG_WARNING(Render, "DIAG-114: gfx submit timeline:{}", text);
                                    }
                                });
                            }
                        }
                        const auto us = [&](auto d) {
                            return static_cast<u64>(
                                std::chrono::duration_cast<std::chrono::microseconds>(d).count());
                        };
                        {
                            std::scoped_lock lk{submit_timing_mutex};
                            ++submit_stats.count;
                            submit_stats.decode_us += us(decoded - *submitted);
                            submit_stats.record_us += us(recorded - *submitted);
                        }
                        // DIAG-GT7-007: the submission's way through the pipe, and its GPU end.
                        static std::atomic<u64> timeline_number{};
                        const u64 number = ++timeline_number;
                        RyDiag::Timeline('S', RyDiag::Seconds(*submitted), RyDiag::Seconds(decoded),
                                         RyDiag::Seconds(recorded), 0.0, number);
                        rasterizer->WhenGpuDone([this, start = *submitted, us, gpu_range, number] {
                            Common::Nvtx::End(gpu_range);
                            const auto gpu_done = std::chrono::steady_clock::now();
                            RyDiag::Timeline('G', RyDiag::Seconds(gpu_done), 0.0, 0.0, 0.0, number);
                            const u64 gpu = us(gpu_done - start);
                            std::scoped_lock lk{submit_timing_mutex};
                            submit_stats.gpu_us += gpu;
                            ++submit_stats.gpu_count;
                            submit_stats.max_gpu_us = std::max(submit_stats.max_gpu_us, gpu);
                        });
                    }
#endif
                    // Isolate the completion wake-up: ETW repeatedly sampled its internal
                    // condition-variable mutex during recorder stalls. Time each boundary
                    // directly so scheduler tracing overhead cannot be mistaken for this call.
#ifndef NDEBUG
                    const auto wake_start = std::chrono::steady_clock::now();
#endif
                    std::unique_lock completion_lock{submit_mutex};
#ifndef NDEBUG
                    const auto wake_locked = std::chrono::steady_clock::now();
#endif
                    submit_cv.notify_all();
#ifndef NDEBUG
                    const auto wake_notified = std::chrono::steady_clock::now();
#endif
                    completion_lock.unlock();
#ifndef NDEBUG
                    const auto wake_released = std::chrono::steady_clock::now();
#endif
#ifndef NDEBUG
                    if (wake_released - wake_start >= std::chrono::milliseconds{1}) {
                        static thread_local std::chrono::steady_clock::time_point next_wake_report{};
                        if (wake_released >= next_wake_report) {
                            next_wake_report = wake_released + std::chrono::seconds{1};
                            const auto ms = [](auto elapsed) {
                                return std::chrono::duration<double, std::milli>(elapsed).count();
                            };
                            LOG_WARNING(Render,
                                "SUBMIT-WAKE: total {:.3f} ms; submit-mutex {:.3f}, notify {:.3f}, unlock {:.3f}; remaining {}",
                                ms(wake_released - wake_start), ms(wake_locked - wake_start),
                                ms(wake_notified - wake_locked), ms(wake_released - wake_notified),
                                num_submits.load());
                        }
                    }
#endif
                });
            }
        }

        // PERF-031: the GPU is idle once everything decoded so far is recorded.
        if (submit_done) {
            submit_done = false;
            // PERF-063: submits the recorded work; reads and writes no guest memory.
            RecordSafe([this] {
                VideoCore::EndCapture();
                if (rasterizer) {
                    rasterizer->OnSubmit();
                    rasterizer->Flush();
                }
            });
        }
        RecordSafe([] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle); });
    }
}

Liverpool::Task Liverpool::ProcessCeUpdate(std::span<const u32> ccb) {
    FIBER_ENTER(ccb_task_name);

    while (!ccb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(ccb.data());
        const u32 type = header->type;
        if (type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", type);
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        const u32 packet_words = header->type3.NumWords() + 1;
        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            // const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::WriteConstRam: {
            const auto* write_const = reinterpret_cast<const PM4WriteConstRam*>(header);
            memcpy(cblock.constants_heap.data() + write_const->Offset(), &write_const->data,
                   write_const->Size());
            break;
        }
        case PM4ItOpcode::DumpConstRam: {
            const auto* dump_const = reinterpret_cast<const PM4DumpConstRam*>(header);
            // PERF-031: queued draws may still read the memory this overwrites.
            SyncRecorder("ce dump");
            if (rasterizer) {
                if (auto* direct = rasterizer->GetBufferCache().GetDirectMemory()) {
                    direct->MarkVolatile(dump_const->Address<VAddr>(), dump_const->Size()); // D4
                }
            }
            memcpy(dump_const->Address<void*>(),
                   cblock.constants_heap.data() + dump_const->Offset(), dump_const->Size());
            // DIAG-009: remember recent dump destinations for the tonemap trace.
            recent_const_dumps[const_dump_sequence % recent_const_dumps.size()] = {
                .address = dump_const->Address<VAddr>(),
                .size = dump_const->Size(),
                .ce_count = cblock.ce_count,
                .de_count = cblock.de_count,
                .sequence = ++const_dump_sequence,
            };
            break;
        }
        case PM4ItOpcode::IncrementCeCounter: {
            ++cblock.ce_count;
            break;
        }
        case PM4ItOpcode::WaitOnDeCounterDiff: {
            const auto diff = it_body[0];
            GpuWaitDiagnostics diagnostics;
            while (ShouldWaitOnDeCounter(cblock.ce_count, cblock.de_count, diff)) {
                if (diagnostics.Ready()) {
                    LOG_WARNING(Render, "GPU CE wait stalled: CE={} DE={} limit={}",
                                cblock.ce_count, cblock.de_count, diff);
                }
                YIELD_CE();
            }
            break;
        }
        case PM4ItOpcode::IndirectBufferConst: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task =
                ProcessCeUpdate({indirect_buffer->Address<const u32>(), indirect_buffer->ib_size});
            RESUME_CE(task);

            while (!task.handle.done()) {
                YIELD_CE();
                RESUME_CE(task);
            }
            task.handle.destroy(); // FIX-054
            break;
        }
        default:
            const u32 count = header->type3.NumWords();
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }
        ccb = NextPacket(ccb, packet_words);
    }

    FIBER_EXIT;
}

Liverpool::Task Liverpool::ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb,
                                           std::span<const u32> original_dcb) {
    FIBER_ENTER(dcb_task_name);
#ifdef RYTY_GPU
    // FIX-GTS-492: GT Sport takes a frame's command memory back at its end-of-pipe interrupt;
    // the prepared draws kept under packets of this memory end then (see EventWriteEop).
    if (Vulkan::NativeRenderer::PreparedLifetimeOn()) {
        // No interrupt for a very long time: the list stays bounded. Retiring early only costs
        // the prepared draws of what is not recorded yet.
        if (decoded_command_ranges.size() >= 16384) {
            if (auto* native = rasterizer ? rasterizer->Native() : nullptr) {
                std::vector<Native::CommandRange> ranges;
                ranges.reserve(decoded_command_ranges.size());
                for (const auto& [begin, end] : decoded_command_ranges) {
                    ranges.push_back({begin, end});
                }
                native->RetirePreparedDraws(ranges);
            }
            decoded_command_ranges.clear();
        }
        for (const auto buffer : {dcb, original_dcb}) {
            if (!buffer.empty()) {
                decoded_command_ranges.emplace_back(
                    reinterpret_cast<u64>(buffer.data()),
                    reinterpret_cast<u64>(buffer.data() + buffer.size()));
            }
        }
    }
#endif
    if (!original_dcb.empty()) {
        RecordCmdBuffer(original_dcb.data(), original_dcb.size_bytes(), false);
        decoding_submit.fetch_add(1, std::memory_order_relaxed); // DIAG-061
#ifdef RYTY_GPU
        if (Native::ShadowEnabled()) {
            Native::ShadowSubmitBase(regs); // NAT-003: the state render jobs start from
        }
#endif
    }
    if (early_fences_pending && std::chrono::steady_clock::now() >= early_fences_at) {
        early_fences_mode = std::exchange(early_fences_pending, 0); // EXP-120
        LOG_WARNING(Render, "EXP-120: early fences on now (mode {})", early_fences_mode);
    }

    cblock.Reset();
    // PERF-019: a command buffer can be reused with other contents; never continue an earlier
    // read-ahead into this one.
    scan_buffer_end = nullptr;
    if (lookahead_outer.empty()) {
        scanned_packets.clear();
    }
    // PERF-023: a nested command buffer an earlier read-ahead already read needs no new one.
    // PERF-031: the read-ahead shares the pipeline cache with the recorder thread; it is off
    // while the draw pipe runs.
    if (on_command_buffer_start && !dcb.empty() && !draw_pipe &&
        !(Common::PerfFeatureEnabled(23) && scanned_packets.contains(dcb.data()))) {
        lookahead_dcb = dcb;
        on_command_buffer_start();
    }
    // PERF-034: with the draw pipe the command thread reads ahead itself, from each command
    // buffer's start while new pipelines keep appearing (PERF-020).
    if (read_ahead_states && !dcb.empty() && !ReadAheadSkipped() &&
        read_ahead_states->MissWithin(std::chrono::seconds{5}) &&
        !(Common::PerfFeatureEnabled(23) && scanned_packets.contains(dcb.data()))) {
        lookahead_dcb = dcb;
        PipeReadAhead();
    }

    // TODO: potentially, ASCs also can depend on CE and in this case the
    // CE task should be moved into more global scope
    Task ce_task{};

    if (!ccb.empty()) {
        // In case of CCB provided kick off CE asap to have the constant heap ready to use
        ce_task = ProcessCeUpdate(ccb);
        RESUME_GFX(ce_task);
    }

    const auto base_addr = reinterpret_cast<uintptr_t>(dcb.data());
    const auto submitted_dcb = dcb;
    const auto live_dcb = original_dcb.empty() ? submitted_dcb : original_dcb;
    live_cmd_buffers.push_back(live_dcb);
    // DIAG-052: the last packets decoded, for an invalid-packet report.
    std::array<std::pair<u32, u32>, 16> recent_packets{};
    u32 num_recent = 0;
    while (!dcb.empty()) {
        ProcessCommands();

        // PERF-GT7-052: let runnable compute submissions decode between graphics packets.
        static const u32 yield_packets = [] {
            const char* env = std::getenv("SHADGT_GFX_YIELD_PACKETS");
            return env && *env ? u32(std::strtoul(env, nullptr, 10)) : 32u;
        }();
        if (yield_packets && ++gfx_packets_since_yield >= yield_packets) {
            gfx_packets_since_yield = 0;
            if (ComputeQueueRunnable()) YIELD_GFX();
        }

        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        const u32 type = header->type;
        recent_packets[num_recent++ % recent_packets.size()] = {
            static_cast<u32>(dcb.data() - submitted_dcb.data()), header->raw};

        switch (type) {
        default:
            UNREACHABLE_MSG("Wrong PM4 type {}", type);
            break;
        case 0: {
            const auto write = DecodeType0RegisterWrite(dcb, regs.reg_array.size());
            if (!write) {
                const size_t word_offset = dcb.data() - submitted_dcb.data();
                const size_t start = word_offset > 16 ? word_offset - 16 : 0;
                const auto context = submitted_dcb.subspan(
                    start, std::min<size_t>(32, submitted_dcb.size() - start));
                LOG_CRITICAL(Render,
                             "Invalid PM4 submission base={:#x}, dword offset={}, context starts "
                             "at dword {}: {:#010x}",
                             base_addr, word_offset, start, fmt::join(context, " "));
                // DIAG-052: the packets before it (dword offset:header), the submission's size,
                // whether the live buffer still holds the same words, and a wider context.
                std::string recent;
                for (u32 i = 0; i < std::min<u32>(num_recent, 16); ++i) {
                    const auto& [offset, raw] =
                        recent_packets[(num_recent - 1 - i) % recent_packets.size()];
                    recent += fmt::format(" {}:{:#010x}", offset, raw);
                }
                const size_t wide_start = word_offset > 96 ? word_offset - 96 : 0;
                const auto wide = submitted_dcb.subspan(
                    wide_start, std::min<size_t>(112, submitted_dcb.size() - wide_start));
                const bool same = live_dcb.size() == submitted_dcb.size() &&
                                  std::equal(live_dcb.begin(), live_dcb.end(), submitted_dcb.begin());
                LOG_CRITICAL(Render,
                             "DIAG-052: submission of {} dwords (copied: {}, live buffer still "
                             "equal: {}); last packets (newest first):{}; dwords from {}: {:#010x}",
                             submitted_dcb.size(), !original_dcb.empty(), same, recent, wide_start,
                             fmt::join(wide, " "));
                // DIAG-052: the emulator's own recent writes near the packets before it.
                const VAddr bad = reinterpret_cast<VAddr>(dcb.data());
                LOG_CRITICAL(Render, "DIAG-052: emulator writes within 256 bytes of {:#x}:{}",
                             bad, Core::Memory::Instance()->DescribeBackingWrites(bad - 128, 256));
            }
            ASSERT_MSG(write.has_value(),
                       "Invalid PM4 type 0 at {:#x}: header={:#x}, remaining dwords={}",
                       reinterpret_cast<uintptr_t>(dcb.data()), header->raw, dcb.size());
            std::memcpy(&regs.reg_array[write->first_register], write->values.data(),
                        write->values.size_bytes());
            MarkRegs(write->first_register, static_cast<u32>(write->values.size()));

            // The graphics queue keeps compute shader state separately from the graphics regs.
            constexpr u32 cs_first = Regs::ShRegWordOffset + 0x200;
            constexpr u32 cs_end = cs_first + sizeof(ComputeProgram) / sizeof(u32);
            const u32 first = std::max(write->first_register, cs_first);
            const u32 end = std::min<u32>(write->first_register + write->values.size(), cs_end);
            if (first < end) {
                auto* cs = reinterpret_cast<u32*>(&mapped_queues[GfxQueueId].cs_state);
                std::memcpy(cs + first - cs_first, &regs.reg_array[first],
                            (end - first) * sizeof(u32));
            }
            dcb = NextPacket(dcb, write->values.size() + 1);
            continue;
        }
        case 2:
            // Type-2 packet are used for padding purposes
            dcb = NextPacket(dcb, 1);
            continue;
        case 3:
            const u32 count = header->type3.NumWords();
            const PM4ItOpcode opcode = header->type3.opcode;
            ++Common::GetWorkCounters().pm4_packets;
            lookahead_dcb =
                dcb.size() > count + 1 ? dcb.subspan(count + 1) : std::span<const u32>{};
            switch (opcode) {
            case PM4ItOpcode::Nop: {
                const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
                if (nop->header.count.Value() == 0) {
                    break;
                }

                switch (nop->data_block[0]) {
                case PM4CmdNop::PayloadType::PatchedFlip: {
                    // There is no evidence that GPU CP drives flip events by parsing
                    // special NOP packets. For convenience lets assume that it does.
                    RecordSafe(
                        [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip); });
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPush: {
#ifndef NDEBUG
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        std::string label{reinterpret_cast<const char*>(&nop->data_block[1]),
                                          marker_sz};
                        RecordSafe([this, label = std::move(label)] {
                            rasterizer->ScopeMarkerBegin(label, true);
                        });
                    }
#endif
                    break;
                }
                case PM4CmdNop::PayloadType::DebugColorMarkerPush: {
#ifndef NDEBUG
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        std::string label{reinterpret_cast<const char*>(&nop->data_block[1]),
                                          marker_sz};
                        const u32 color = *reinterpret_cast<const u32*>(
                            reinterpret_cast<const u8*>(&nop->data_block[1]) + marker_sz);
                        RecordSafe([this, label = std::move(label), color] {
                            rasterizer->ScopedMarkerInsertColor(label, color, true);
                        });
                    }
#endif
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPop: {
#ifndef NDEBUG
                    if (guest_markers_enabled) {
                        RecordSafe([this] { rasterizer->ScopeMarkerEnd(true); });
                    }
#endif
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::ContextControl: {
                break;
            }
            case PM4ItOpcode::ClearState: {
                regs.SetDefaults();
                regs_dirty.MarkAll();
                pipeline_regs_written = true;
                break;
            }
            case PM4ItOpcode::SetConfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ConfigRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);
                std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));
                MarkRegs(reg_addr, count - 1);
                break;
            }
            case PM4ItOpcode::SetContextReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ContextRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);

                std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));
                MarkRegs(reg_addr, count - 1);

                // In the case of HW, render target memory has alignment as color block operates on
                // tiles. There is no information of actual resource extents stored in CB context
                // regs, so any deduction of it from slices/pitch will lead to a larger surface
                // created. The same applies to the depth targets. Fortunately, the guest always
                // sends a trailing NOP packet right after the context regs setup, so we can use the
                // heuristic below and extract the hint to determine actual resource dims.

                switch (reg_addr) {
                case ContextRegs::CbColor0Base:
                case ContextRegs::CbColor1Base:
                case ContextRegs::CbColor2Base:
                case ContextRegs::CbColor3Base:
                case ContextRegs::CbColor4Base:
                case ContextRegs::CbColor5Base:
                case ContextRegs::CbColor6Base:
                case ContextRegs::CbColor7Base: {
                    const auto col_buf_id = (reg_addr - ContextRegs::CbColor0Base) /
                                            (ContextRegs::CbColor1Base - ContextRegs::CbColor0Base);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x0e || nop_offset == 0x0d || nop_offset == 0x0b) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    } else {
                        last_cb_extent[col_buf_id].raw = 0;
                    }
                    break;
                }
                case ContextRegs::CbColor0Cmask:
                case ContextRegs::CbColor1Cmask:
                case ContextRegs::CbColor2Cmask:
                case ContextRegs::CbColor3Cmask:
                case ContextRegs::CbColor4Cmask:
                case ContextRegs::CbColor5Cmask:
                case ContextRegs::CbColor6Cmask:
                case ContextRegs::CbColor7Cmask: {
                    const auto col_buf_id =
                        (reg_addr - ContextRegs::CbColor0Cmask) /
                        (ContextRegs::CbColor1Cmask - ContextRegs::CbColor0Cmask);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x04) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    }
                    break;
                }
                case ContextRegs::DbZInfo: {
                    if (header->type3.count == 8) {
                        ASSERT_MSG(payload[20] == 0xc0001000,
                                   "NOP hint is missing in DB setup sequence");
                        last_db_extent.raw = payload[21];
                    } else {
                        last_db_extent.raw = 0;
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::SetShReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto set_size = (count - 1) * sizeof(u32);

                if (set_data->reg_offset >= 0x200 &&
                    set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                    ASSERT(set_size <= sizeof(ComputeProgram));
                    auto* addr = reinterpret_cast<u32*>(&mapped_queues[GfxQueueId].cs_state) +
                                 (set_data->reg_offset - 0x200);
                    std::memcpy(addr, header + 2, set_size);
                } else {
                    std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                                header + 2, set_size);
                    MarkRegs(Regs::ShRegWordOffset + set_data->reg_offset, count - 1);
                }
                break;
            }
            case PM4ItOpcode::SetUconfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                std::memcpy(&regs.reg_array[Regs::UconfigRegWordOffset + set_data->reg_offset],
                            header + 2, (count - 1) * sizeof(u32));
                MarkRegs(Regs::UconfigRegWordOffset + set_data->reg_offset, count - 1);
                break;
            }
            case PM4ItOpcode::SetPredication: {
                // Keep the unsupported command visible without formatting and
                // enqueueing the same warning hundreds of times per second.
                static thread_local u64 packets = 0;
                const auto total = ++packets;
                if (total <= 3 || total % 4096 == 0)
                    LOG_WARNING(Render, "Unimplemented IT_SET_PREDICATION ({} packets)", total);
                break;
            }
            case PM4ItOpcode::IndexType: {
                const auto* index_type = reinterpret_cast<const PM4CmdDrawIndexType*>(header);
                regs.index_buffer_type.raw = index_type->raw;
                MarkReg(regs.index_buffer_type);
                break;
            }
            case PM4ItOpcode::DrawIndex2: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
                regs.max_index_size = draw_index->max_size;
                regs.index_base_address.base_addr_lo = draw_index->index_base_lo;
                regs.index_base_address.base_addr_hi = draw_index->index_base_hi;
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                MarkReg(regs.max_index_size);
                MarkReg(regs.index_base_address);
                MarkReg(regs.num_indices);
                MarkReg(regs.draw_initiator);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
#ifdef RYTY_GPU
                if (Native::ShadowComparing()) {
                    Native::ShadowCheckDraw(header, regs); // NAT-003
                }
#endif
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                staged_index_offset = 0; // MT-005
                decode_draw_packet = cmd_address; // OPT-012
                RecordDraw(
                    [this, cmd_address] {
                        rasterizer->ScopeMarker("gfx:{}:DrawIndex2",
                                                fmt::make_format_args(cmd_address),
                                                [&] {
                                                    // NAT-032: shadow checks key by it
                                                    Vulkan::Rasterizer::t_draw_packet = cmd_address;
                                                    rasterizer->Draw(true);
                                                    Vulkan::Rasterizer::t_draw_packet = nullptr;
                                                });
                    },
                    false, true);
                break;
            }
            case PM4ItOpcode::DrawIndexOffset2: {
                const auto* draw_index_off =
                    reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
                regs.max_index_size = draw_index_off->max_size;
                regs.num_indices = draw_index_off->index_count;
                regs.draw_initiator = draw_index_off->draw_initiator;
                MarkReg(regs.max_index_size);
                MarkReg(regs.num_indices);
                MarkReg(regs.draw_initiator);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
#ifdef RYTY_GPU
                if (Native::ShadowComparing()) {
                    Native::ShadowCheckDraw(header, regs); // NAT-003
                }
#endif
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const u32 index_offset = draw_index_off->index_offset;
                staged_index_offset = index_offset; // MT-005
                decode_draw_packet = cmd_address; // OPT-012
                RecordDraw(
                    [this, cmd_address, index_offset] {
                        rasterizer->ScopeMarker("gfx:{}:DrawIndexOffset2",
                                                fmt::make_format_args(cmd_address),
                                                [&] {
                                                    // NAT-032: shadow checks key by it
                                                    Vulkan::Rasterizer::t_draw_packet = cmd_address;
                                                    rasterizer->Draw(true, index_offset);
                                                    Vulkan::Rasterizer::t_draw_packet = nullptr;
                                                });
                    },
                    false, true);
                break;
            }
            case PM4ItOpcode::DrawIndexAuto: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                MarkReg(regs.num_indices);
                MarkReg(regs.draw_initiator);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
#ifdef RYTY_GPU
                if (Native::ShadowComparing()) {
                    Native::ShadowCheckDraw(header, regs); // NAT-003
                }
#endif
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                decode_draw_packet = cmd_address; // OPT-012
                RecordDraw(
                    [this, cmd_address] {
                        rasterizer->ScopeMarker("gfx:{}:DrawIndexAuto",
                                                fmt::make_format_args(cmd_address),
                                                [&] {
                                                    // NAT-032: shadow checks key by it
                                                    Vulkan::Rasterizer::t_draw_packet = cmd_address;
                                                    rasterizer->Draw(false);
                                                    Vulkan::Rasterizer::t_draw_packet = nullptr;
                                                });
                    },
                    false, true);
                break;
            }
            case PM4ItOpcode::DrawIndirect: {
                const auto* draw_indirect = reinterpret_cast<const PM4CmdDrawIndirect*>(header);
                const auto offset = draw_indirect->data_offset;
                const auto stride = sizeof(DrawIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const VAddr args_addr = indirect_args_addr;
                const u16 base_vtx_loc = draw_indirect->base_vtx_loc;
                const u16 start_inst_loc = draw_indirect->start_inst_loc;
                const IndirectSgprs indirect_sgprs{base_vtx_loc, start_inst_loc}; // PERF-GT7-031
                RecordDraw(
                    [this, cmd_address, args_addr, offset, stride, base_vtx_loc, start_inst_loc] {
                        rasterizer->ScopeMarker(
                            "gfx:{}:DrawIndirect", fmt::make_format_args(cmd_address), [&] {
                                rasterizer->DrawIndirect(false, args_addr, offset, stride, 1, 0,
                                                         base_vtx_loc, start_inst_loc);
                            });
                    },
                    false, false, &indirect_sgprs);
                break;
            }
            case PM4ItOpcode::DrawIndirectMulti: {
                const auto* draw_indirect =
                    reinterpret_cast<const PM4CmdDrawIndirectMulti*>(header);
                const auto offset = draw_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const VAddr args_addr = indirect_args_addr;
                const u32 stride = draw_indirect->stride;
                const u32 max_count = draw_indirect->count;
                const u16 base_vtx_loc = draw_indirect->base_vtx_loc;
                const u16 start_inst_loc = draw_indirect->start_inst_loc;
                const IndirectSgprs indirect_sgprs{base_vtx_loc, start_inst_loc}; // PERF-GT7-031
                RecordDraw(
                    [this, cmd_address, args_addr, offset, stride, max_count, base_vtx_loc,
                     start_inst_loc] {
                        rasterizer->ScopeMarker(
                            "gfx:{}:DrawIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                                rasterizer->DrawIndirect(false, args_addr, offset, stride,
                                                         max_count, 0, base_vtx_loc,
                                                         start_inst_loc);
                            });
                    },
                    false, false, &indirect_sgprs);
                break;
            }
            case PM4ItOpcode::DrawIndexIndirect: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirect*>(header);
                const auto offset = draw_index_indirect->data_offset;
                const auto stride = sizeof(DrawIndexedIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const VAddr args_addr = indirect_args_addr;
                const u16 base_vtx_loc = draw_index_indirect->base_vtx_loc;
                const u16 start_inst_loc = draw_index_indirect->start_inst_loc;
                const IndirectSgprs indirect_sgprs{base_vtx_loc, start_inst_loc}; // PERF-GT7-031
                RecordDraw(
                    [this, cmd_address, args_addr, offset, stride, base_vtx_loc, start_inst_loc] {
                        rasterizer->ScopeMarker(
                            "gfx:{}:DrawIndexIndirect", fmt::make_format_args(cmd_address), [&] {
                                rasterizer->DrawIndirect(true, args_addr, offset, stride, 1, 0,
                                                         base_vtx_loc, start_inst_loc);
                            });
                    },
                    false, false, &indirect_sgprs);
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const VAddr args_addr = indirect_args_addr;
                const u32 stride = draw_index_indirect->stride;
                const u32 max_count = draw_index_indirect->count;
                const u16 base_vtx_loc = draw_index_indirect->base_vtx_loc;
                const u16 start_inst_loc = draw_index_indirect->start_inst_loc;
                const IndirectSgprs indirect_sgprs{base_vtx_loc, start_inst_loc}; // PERF-GT7-031
                RecordDraw(
                    [this, cmd_address, args_addr, offset, stride, max_count, base_vtx_loc,
                     start_inst_loc] {
                        rasterizer->ScopeMarker("gfx:{}:DrawIndexIndirectMulti",
                                                fmt::make_format_args(cmd_address), [&] {
                                                    rasterizer->DrawIndirect(
                                                        true, args_addr, offset, stride, max_count,
                                                        0, base_vtx_loc, start_inst_loc);
                                                });
                    },
                    false, false, &indirect_sgprs);
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectCountMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectCountMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const VAddr args_addr = indirect_args_addr;
                const u32 stride = draw_index_indirect->stride;
                const u32 max_count = draw_index_indirect->count;
                const VAddr count_addr = draw_index_indirect->count_indirect_enable.Value()
                                             ? draw_index_indirect->count_addr
                                             : 0;
                const u16 base_vtx_loc = draw_index_indirect->base_vtx_loc;
                const u16 start_inst_loc = draw_index_indirect->start_inst_loc;
                const IndirectSgprs indirect_sgprs{base_vtx_loc, start_inst_loc}; // PERF-GT7-031
                RecordDraw(
                    [this, cmd_address, args_addr, offset, stride, max_count, count_addr,
                     base_vtx_loc, start_inst_loc] {
                        rasterizer->ScopeMarker("gfx:{}:DrawIndexIndirectCountMulti",
                                                fmt::make_format_args(cmd_address), [&] {
                                                    rasterizer->DrawIndirect(
                                                        true, args_addr, offset, stride, max_count,
                                                        count_addr, base_vtx_loc, start_inst_loc);
                                                });
                    },
                    false, false, &indirect_sgprs);
                break;
            }
            case PM4ItOpcode::DispatchDirect: {
                const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
                auto& cs_program = GetCsRegs();
                cs_program.dim_x = dispatch_direct->dim_x;
                cs_program.dim_y = dispatch_direct->dim_y;
                cs_program.dim_z = dispatch_direct->dim_z;
                cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                RecordDraw(
                    [this, cmd_address] {
                        rasterizer->ScopeMarker("gfx:{}:DispatchDirect",
                                                fmt::make_format_args(cmd_address),
                                                [&] { rasterizer->DispatchDirect(); });
                    },
                    true, true);
                break;
            }
            case PM4ItOpcode::DispatchIndirect: {
                const auto* dispatch_indirect =
                    reinterpret_cast<const PM4CmdDispatchIndirect*>(header);
                auto& cs_program = GetCsRegs();
                const auto offset = dispatch_indirect->data_offset;
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const VAddr args_addr = indirect_args_addr;
                RecordDraw(
                    [this, cmd_address, args_addr, offset, size] {
                        rasterizer->ScopeMarker(
                            "gfx:{}:DispatchIndirect", fmt::make_format_args(cmd_address),
                            [&] { rasterizer->DispatchIndirect(args_addr, offset, size); });
                    },
                    true);
                break;
            }
            case PM4ItOpcode::NumInstances: {
                const auto* num_instances = reinterpret_cast<const PM4CmdDrawNumInstances*>(header);
                regs.num_instances.num_instances = num_instances->num_instances;
                MarkReg(regs.num_instances);
                break;
            }
            case PM4ItOpcode::IndexBase: {
                const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
                regs.index_base_address.base_addr_lo = index_base->addr_lo;
                regs.index_base_address.base_addr_hi = index_base->addr_hi;
                MarkReg(regs.index_base_address);
                break;
            }
            case PM4ItOpcode::IndexBufferSize: {
                const auto* index_size = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header);
                regs.num_indices = index_size->num_indices;
                MarkReg(regs.num_indices);
                break;
            }
            case PM4ItOpcode::SetBase: {
                const auto* set_base = reinterpret_cast<const PM4CmdSetBase*>(header);
                ASSERT(set_base->base_index == PM4CmdSetBase::BaseIndex::DrawIndexIndirPatchTable);
                indirect_args_addr = set_base->Address<u64>();
                break;
            }
            case PM4ItOpcode::EventWrite: {
                const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
                LOG_DEBUG(Render, "Encountered EventWrite: event_type = {}, event_index = {}",
                          magic_enum::enum_name(event->event_type.Value()),
                          magic_enum::enum_name(event->event_index.Value()));
                if (event->event_type.Value() == EventType::SoVgtStreamoutFlush) {
                    // TODO: handle proper synchronization, for now signal that update is done
                    // immediately
                    regs.cp_strmout_cntl.offset_update_done = 1;
                    MarkReg(regs.cp_strmout_cntl);
                } else if (event->event_index.Value() == EventIndex::ZpassDone) {
                    if (event->event_type.Value() == EventType::PixelPipeStatDump) {
                        static constexpr u64 OcclusionCounterValidMask = 0x8000000000000000ULL;
                        static constexpr u64 OcclusionCounterStep = 0x2FFFFFFULL;
                        u64* results = event->Address<u64*>();
                        const u64 counter = pixel_counter;
                        pixel_counter += OcclusionCounterStep;
                        // PERF-031: written in order with the draws before it.
                        const auto write_results = [results, counter, pairs = num_counter_pairs] {
#ifdef RYTY_GPU
                            // FIX-072: GT Sport races crashed on the recorder writing these
                            // results to addresses that were not (or no longer) mapped
                            // (0x13ac1008, 0x17f60c0). Such a write is dropped.
                            if (!Core::RytyMemory::IsWritableTarget(
                                    reinterpret_cast<VAddr>(results),
                                    u64(pairs) * 2 * sizeof(u64))) {
                                static std::atomic<u32> dropped{};
                                if (dropped.fetch_add(1) < 20) {
                                    LOG_WARNING(Render,
                                                "FIX-072: occlusion results at {:#x} not written: "
                                                "not writable memory",
                                                reinterpret_cast<VAddr>(results));
                                }
                                return;
                            }
#endif
                            u64* result = results;
                            for (s32 i = 0; i < s32(pairs); ++i, result += 2) {
                                *result = counter | OcclusionCounterValidMask;
                            }
                        };
                        // PERF-116 (SHADGT_OCCLUSION_AT_DECODE=1): the values do not depend on
                        // the draws, so they are written now; recorded, they were a barrier for
                        // the buffer stage (~450 a second in races).
                        static const bool write_at_decode = [] {
                            const char* env = std::getenv("SHADGT_OCCLUSION_AT_DECODE");
                            return env && env[0] == '1';
                        }();
                        if (early_fences_mode || write_at_decode) {
                            // PERF-063: the values do not depend on the draws; written now, so
                            // the window's fence may be signaled at decode.
                            write_results();
                        } else {
                            Record(write_results, "occlusion results");
                        }
                    }
                }
                break;
            }
            case PM4ItOpcode::EventWriteEos: {
                const auto event = *reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                if (draw_pipe && rasterizer &&
                    event.command != PM4CmdEventWriteEos::Command::GdsStore &&
                    (EarlyFencesActive() || EarlyFenceAllowed())) {
                    // EXP-063: signaled now; the recorder only submits at this point.
                    event.SignalFence([](void* address, u64 data, u32 num_bytes) {
                        Core::Memory::Instance()->TryWriteBacking(address, &data, num_bytes);
                    });
                    ++g_fences_early;
                    RecordSafe(
                        [this] { rasterizer->OnFence(Vulkan::Rasterizer::DrainSource::GfxEos); });
                    if (early_fences_mode) {
                        NoteEarlyFence();
                    }
                    break;
                }
                // PERF-031: fences are signaled in order behind the recorded work.
                RecordLabelWrite(
                    event.command != PM4CmdEventWriteEos::Command::GdsStore ? event.Address<VAddr>()
                                                                            : 0,
                    FenceValueBytes(DataSelect::Data32Low, event.DataDWord(), 0), [this, event] {
                        const auto* event_eos = &event;
                        const bool deferred =
                            rasterizer &&
                            event_eos->command != PM4CmdEventWriteEos::Command::GdsStore &&
                            rasterizer->DeferFenceSignal(
                                event_eos->Address<VAddr>(),
                                [event] {
                                    event.SignalFence([](void* address, u64 data, u32 num_bytes) {
                                        WriteDeferredFence(address, &data, num_bytes);
                                    });
                                },
                                false,
                                FenceValueBytes(DataSelect::Data32Low, event_eos->DataDWord(), 0),
                                false); // D5: rides the next submit
                        if (!deferred) {
                            if (rasterizer) {
                                rasterizer->OnFence(Vulkan::Rasterizer::DrainSource::GfxEos);
                            }
                            event_eos->SignalFence([](void* address, u64 data, u32 num_bytes) {
                                auto* memory = Core::Memory::Instance();
                                ASSERT(memory->TryWriteBacking(address, &data, num_bytes));
                            });
                        }
                        if (event_eos->command == PM4CmdEventWriteEos::Command::GdsStore) {
                            ASSERT(event_eos->size == 1);
                            if (rasterizer) {
                                rasterizer->FinishForGds();
                                const u32 value = rasterizer->ReadDataFromGds(event_eos->gds_index);
                                *event_eos->Address() = value;
                            }
                        }
                    });
                break;
            }
            case PM4ItOpcode::EventWriteEop: {
                const auto event = *reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                const u64 eop_submit = decoding_submit.load(std::memory_order_relaxed);
                NoteEopStage(eop_submit, 0); // DIAG-SGT-074: decoded
                const bool frame_eop = event.int_sel.Value() != InterruptSelect::None;
                if (frame_eop) {
                    push_occurrences.clear(); // FEAT-SGT-079 step 17: a frame ends
                    push_key = 0;
                }
                // FIX-GTS-492: when this event raises the interrupt, the game takes back the
                // command memory decoded so far (whole buffers, the rest of this one included):
                // the prepared draws in it are dropped where the event is signaled, before its
                // label is written (a game may poll it) and before the interrupt.
                std::function<void()> retire_prepared = [] {};
#ifdef RYTY_GPU
                if (auto* native = rasterizer ? rasterizer->Native() : nullptr;
                    native && !decoded_command_ranges.empty() &&
                    event.int_sel.Value() != InterruptSelect::None) {
                    auto ranges = std::make_shared<std::vector<Native::CommandRange>>();
                    ranges->reserve(decoded_command_ranges.size());
                    for (const auto& [begin, end] : decoded_command_ranges) {
                        ranges->push_back({begin, end});
                    }
                    decoded_command_ranges.clear();
                    retire_prepared = [native, ranges] { native->RetirePreparedDraws(*ranges); };
                }
#endif
                if (draw_pipe && rasterizer && (EarlyFencesActive() || EarlyFenceAllowed())) {
                    // EXP-063: signaled now; the recorder only submits at this point.
                    retire_prepared();
                    event.SignalFence(
                        [](void* address, u64 data, u32 num_bytes) {
                            Core::Memory::Instance()->TryWriteBacking(address, &data, num_bytes);
                        },
                        [eop_submit] {
                            NoteEop(eop_submit);
                            Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop);
                        });
                    ++g_fences_early;
                    RecordSafe(
                        [this] { rasterizer->OnFence(Vulkan::Rasterizer::DrainSource::GfxEop); });
                    if (early_fences_mode) {
                        NoteEarlyFence();
                    }
                    break;
                }
                // PERF-031: fences are signaled in order behind the recorded work.
                RecordLabelWrite(
                    reinterpret_cast<VAddr>(event.Address<u32>()),
                    FenceValueBytes(event.data_sel.Value(), event.DataDWord(), event.DataQWord()),
                    [this, event, eop_submit, retire_prepared, frame_eop] {
                        NoteEopStage(eop_submit, 1); // DIAG-SGT-074: the recorder got here
#ifdef RYTY_GPU
                        if (frame_eop && rasterizer) { // FEAT-SGT-079 step 17
                            if (auto* native = rasterizer->Native()) native->LearnedFrameEnd();
                        }
#endif
                        if (EopLatencyDiag() && eop_submit && rasterizer) {
                            g_eop_tick[eop_submit % 1024].store(
                                rasterizer->GetScheduler().CurrentTick(), std::memory_order_relaxed);
                        }
                        const auto* event_eop = &event;
                        const bool deferred =
                            rasterizer &&
                            rasterizer->DeferFenceSignal(
                                reinterpret_cast<VAddr>(event_eop->Address<u32>()),
                                [event, eop_submit, retire_prepared] {
                                    retire_prepared();
                                    event.SignalFence(
                                        [](void* address, u64 data, u32 num_bytes) {
                                            WriteDeferredFence(address, &data, num_bytes);
                                        },
                                        [eop_submit] {
                                            NoteEop(eop_submit);
                                            #ifndef NDEBUG
++g_eops_deferred;
#endif
                                            Platform::IrqC::Instance()->Signal(
                                                Platform::InterruptId::GfxEop);
                                        });
                                },
                                false,
                                FenceValueBytes(event_eop->data_sel.Value(), event_eop->DataDWord(),
                                                event_eop->DataQWord()),
                                true); // D5: EOP submits; labels before it ride along
                        if (!deferred) {
                            if (rasterizer) {
                                rasterizer->OnFence(Vulkan::Rasterizer::DrainSource::GfxEop);
                            }
                            retire_prepared();
                            event_eop->SignalFence(
                                [](void* address, u64 data, u32 num_bytes) {
                                    auto* memory = Core::Memory::Instance();
                                    ASSERT(memory->TryWriteBacking(address, &data, num_bytes));
                                },
                                [eop_submit] {
                                    NoteEop(eop_submit);
                                    #ifndef NDEBUG
++g_eops_immediate;
#endif
                                    Platform::IrqC::Instance()->Signal(
                                        Platform::InterruptId::GfxEop);
                                });
                        }
                    });
                break;
            }
            case PM4ItOpcode::DmaData: {
                const auto* dma_packet = reinterpret_cast<const PM4DmaData*>(header);
                if (dma_packet->dst_addr_lo == 0x3022C || !rasterizer) {
                    break;
                }
                if (SkipRegisterSpaceDmaData(dma_packet, "gfx")) {
                    break;
                }
                // PERF-031: copies run in order with the draws around them. One that writes
                // a command buffer still being decoded runs before decoding goes on.
                const auto dma = *dma_packet;
                const bool to_memory =
                    dma.dst_sel == DmaDataDst::Memory || dma.dst_sel == DmaDataDst::MemoryUsingL2;
                const bool src_memory = dma.src_sel == DmaDataSrc::Memory ||
                                        dma.src_sel == DmaDataSrc::MemoryUsingL2;
                if (to_memory) {
                    WatchWrite(dma.DstAddress<VAddr>(), dma.NumBytes(),
                               src_memory ? dma.SrcAddress<VAddr>() : 0, "gfx DMA_DATA");
                }
                RecordDma([this, dma] {
                    const auto* dma_data = &dma;
                    if (dma_data->src_sel == DmaDataSrc::Data &&
                        dma_data->dst_sel == DmaDataDst::Gds) {
                        rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(),
                                               dma_data->data, true);
                    } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                                dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                               dma_data->dst_sel == DmaDataDst::Gds) {
                        rasterizer->CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                                               dma_data->NumBytes(), true, false);
                    } else if (dma_data->src_sel == DmaDataSrc::Data &&
                               (dma_data->dst_sel == DmaDataDst::Memory ||
                                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                        rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                                               dma_data->data, false);
                    } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                               (dma_data->dst_sel == DmaDataDst::Memory ||
                                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                        rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                                               dma_data->NumBytes(), false, true);
                    } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                                dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                               (dma_data->dst_sel == DmaDataDst::Memory ||
                                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                        rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(),
                                               dma_data->SrcAddress<VAddr>(), dma_data->NumBytes(),
                                               false, false);
                    } else {
                        UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}",
                                        u32(dma_data->src_sel), u32(dma_data->dst_sel));
                    }
                }, src_memory ? dma.SrcAddress<VAddr>() : 0, src_memory ? dma.NumBytes() : 0,
                          to_memory ? dma.DstAddress<VAddr>() : 0, to_memory ? dma.NumBytes() : 0);
                if (to_memory && WritesLiveCommands(dma.DstAddress<VAddr>(), dma.NumBytes())) {
                    SyncRecorder("dma into commands");
                }
                break;
            }
            case PM4ItOpcode::WriteData: {
                const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
                ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
                const u32 data_size = (header->type3.count.Value() - 2) * 4;
                u64* address = write_data->Address<u64*>();
                WatchWrite(reinterpret_cast<VAddr>(address), data_size, 0, "gfx WRITE_DATA");
                if (rasterizer) {
                    if (auto* direct = rasterizer->GetBufferCache().GetDirectMemory()) {
                        direct->MarkVolatile(reinterpret_cast<VAddr>(address), data_size); // D4
                    }
                }
                if (!write_data->wr_one_addr.Value()) {
                    // Keep guest-visible writes in order behind deferred fences.
                    std::vector<u8> data(data_size);
                    std::memcpy(data.data(), write_data->data, data_size);
                    // PERF-031: in order with the recorded work; before decoding goes on when it
                    // writes a command buffer being decoded.
                    const bool into_commands =
                        WritesLiveCommands(reinterpret_cast<VAddr>(address), data_size);
                    std::vector<u8> label = data;
                    if (!into_commands &&
                        TryEarlyLabel(reinterpret_cast<VAddr>(address), data,
                                      u32(Vulkan::Rasterizer::DrainSource::GfxWriteData))) {
                        break; // PERF-069: written at decode
                    }
                    RecordLabelWrite(
                        reinterpret_cast<VAddr>(address), std::move(label),
                        [this, address, data = std::move(data)]() mutable {
#ifdef RYTY_GPU
                            // NAT-100: native mode writes it on the GPU in command order.
                            if (rasterizer && rasterizer->NativeInlineWrite(
                                                  reinterpret_cast<VAddr>(address), data)) {
                                return;
                            }
#endif
                            const u32 data_size = static_cast<u32>(data.size());
                            const std::vector<u8> value = data;
                            const bool deferred =
                                rasterizer &&
                                rasterizer->DeferFenceSignal(
                                    reinterpret_cast<VAddr>(address),
                                    [address, data = std::move(data)] {
                                        WriteDeferredFence(address, data.data(), u32(data.size()));
                                    },
                                    false, value, false); // D5: rides the next submit
                            if (!deferred) {
                                if (rasterizer) {
                                    rasterizer->OnFence(
                                        Vulkan::Rasterizer::DrainSource::GfxWriteData);
                                }
                                Core::MemoryManager::NoteEmulatorWrite(
                                    reinterpret_cast<VAddr>(address), data_size, value.data());
                                std::memcpy(address, value.data(), data_size);
                                VideoCore::BumpUploadEpoch();
                            } else {
                                rasterizer->InlineDeferredWrite(reinterpret_cast<VAddr>(address),
                                                                value);
                            }
                        });
                    if (into_commands) {
                        SyncRecorder("write into commands");
                    }
                } else {
                    UNREACHABLE();
                }
                break;
            }
            case PM4ItOpcode::CopyData: {
                const auto* copy_data = reinterpret_cast<const PM4CmdCopyData*>(header);
                LOG_WARNING(Render,
                            "unhandled IT_COPY_DATA src_sel = {}, dst_sel = {}, "
                            "count_sel = {}, wr_confirm = {}, engine_sel = {}",
                            u32(copy_data->src_sel.Value()), u32(copy_data->dst_sel.Value()),
                            copy_data->count_sel.Value(), copy_data->wr_confirm.Value(),
                            u32(copy_data->engine_sel.Value()));
                break;
            }
            case PM4ItOpcode::MemSemaphore: {
                const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
                if (mem_semaphore->IsSignaling()) {
                    Record([semaphore = *mem_semaphore] { semaphore.Signal(); }, "memory semaphore");
                } else {
                    GpuWaitDiagnostics diagnostics;
                    const auto wait_start = FrontendWaitNow();
                    if (!mem_semaphore->Signaled()) {
                        SyncRecorder("mem semaphore");
                    }
                    const bool waited = !mem_semaphore->Signaled();
                    while (!mem_semaphore->Signaled()) {
                        if (diagnostics.Ready()) {
                            LOG_WARNING(Render,
                                        "GPU semaphore wait stalled: address={:#x} value={}",
                                        mem_semaphore->Address<uintptr_t>(),
                                        *mem_semaphore->Address<u64*>());
                        }
                        YIELD_GFX();
                    }
                    if (waited) {
                        RecordFrontendWait(FrontendWait::GfxMemSemaphore,
                                           mem_semaphore->Address<uintptr_t>(), wait_start);
                    }
                    mem_semaphore->Decrement();
                }
                break;
            }
            case PM4ItOpcode::AcquireMem: {
                // const auto* acquire_mem = reinterpret_cast<PM4CmdAcquireMem*>(header);
                break;
            }
            case PM4ItOpcode::Rewind: {
                if (!rasterizer) {
                    break;
                }
                const size_t rewind_offset = dcb.data() - submitted_dcb.data();
                ASSERT_MSG(live_dcb.size() == submitted_dcb.size() && dcb.size() >= 2,
                           "Invalid REWIND packet or original command-buffer size");
                GpuWaitDiagnostics diagnostics;
                const auto snapshot =
                    std::span{const_cast<u32*>(submitted_dcb.data()), submitted_dcb.size()};
                const auto rewind_start = FrontendWaitNow();
                const bool rewind_waited =
                    !RefreshRewindTailIfReady(snapshot, live_dcb, rewind_offset);
                while (rewind_waited &&
                       !RefreshRewindTailIfReady(snapshot, live_dcb, rewind_offset)) {
                    if (diagnostics.Ready()) {
                        LOG_WARNING(Render, "GPU REWIND stalled: snapshot={:#x} live={:#x}",
                                    reinterpret_cast<uintptr_t>(header),
                                    reinterpret_cast<uintptr_t>(live_dcb.data() + rewind_offset));
                    }
                    YIELD_GFX();
                }
                if (rewind_waited) {
                    RecordFrontendWait(FrontendWait::GfxRewind, reinterpret_cast<uintptr_t>(header),
                                       rewind_start);
                }
                break;
            }
            case PM4ItOpcode::WaitRegMem: {
                const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                // ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
                // Optimization: VO label waits are special because the emulator
                // will write to the label when presentation is finished. So if
                // there are no other submits to yield to we can sleep the thread
                // instead and allow other tasks to run.
                const u64* wait_addr = wait_reg_mem->Address<u64*>();
                const auto report_wait = [&] {
                    const bool memory =
                        wait_reg_mem->mem_space == PM4CmdWaitRegMem::MemSpace::Memory;
                    const u32 value =
                        memory ? *wait_reg_mem->Address() : regs.reg_array[wait_reg_mem->Reg()];
                    LOG_WARNING(Render,
                                "GPU WAIT_REG_MEM stalled: memory={} address={:#x} value={:#x} "
                                "reference={:#x} mask={:#x} function={}",
                                memory, reinterpret_cast<uintptr_t>(wait_addr), value,
                                wait_reg_mem->ref, wait_reg_mem->mask,
                                u32(wait_reg_mem->function.Value()));
                };
                const auto wait_start = FrontendWaitNow();
#ifndef NDEBUG
                bool diag_drained = false; // DIAG-SGT-027
#endif
                // PERF-031: what is waited on may be written by recorded work (a fence, a copy, a
                // flip); let the recorder finish before waiting.
                if (!wait_reg_mem->Test(regs.reg_array)) {
                    // PERF-048: the label may come from another queue's packets not decoded
                    // yet. This queue stays at its wait while the others take a few turns;
                    // a write they queue moves the wait to the recorder.
                    bool yielded = false;
                    for (u32 turn = 0; draw_pipe && WaitTurnsEnabled() && turn < 4 &&
                                       !wait_reg_mem->Test(regs.reg_array) &&
                                       !PendingLabelSatisfies(*wait_reg_mem);
                         ++turn) {
                        yielded = true;
                        YIELD_GFX();
                    }
                    if (!wait_reg_mem->Test(regs.reg_array) &&
                        PendingLabelSatisfies(*wait_reg_mem)) {
                        // A queued fence writes the label: the recorder waits for it in order
                        // and decoding goes on.
                        ++waits_moved;
                        waits_moved_after_turns += yielded;
#ifndef NDEBUG
                        // DIAG-054: the commands decoded after the wait, as read now; checked
                        // again once the wait is really satisfied.
                        const u32* after = dcb.data() + header->type3.NumWords() + 1;
                        const size_t after_dwords = std::min<size_t>(
                            512, dcb.size() > header->type3.NumWords() + 1
                                     ? dcb.size() - header->type3.NumWords() - 1
                                     : 0);
                        const u64 after_hash = XXH3_64bits(after, after_dwords * sizeof(u32));
                        RecordSafe([this, wait = *wait_reg_mem, after, after_dwords,
                                    after_hash] {
                            RecorderWaitRegMem(wait);
                            ++diag_moved_waits_checked;
                            if (XXH3_64bits(after, after_dwords * sizeof(u32)) != after_hash) {
                                static std::atomic<u32> logged{};
                                ++diag_moved_waits_changed;
                                if (logged.fetch_add(1) < 20) {
                                    LOG_ERROR(Render,
                                              "DIAG-054: commands after a moved WAIT_REG_MEM "
                                              "(label {:#x}) changed between decoding and the "
                                              "label being written: {:#x}+{} dwords",
                                              reinterpret_cast<uintptr_t>(wait.Address<u32*>()),
                                              reinterpret_cast<uintptr_t>(after), after_dwords);
                                }
                            }
                        });
#else
                        RecordSafe([this, wait = *wait_reg_mem] { RecorderWaitRegMem(wait); });
#endif
                        break;
                    }
                    // PERF-GT7-046: until the recorder has run dry, other queues decode.
                    while (WaitsYieldToRecorder() && !wait_reg_mem->Test(regs.reg_array) &&
                           !draw_pipe->QueueIdle()) {
                        YIELD_GFX();
                    }
                    if (!wait_reg_mem->Test(regs.reg_array)) {
                        SyncRecorder("wait reg mem");
#ifndef NDEBUG
                        ++wait_drains[0].packets; // DIAG-SGT-027
                        diag_drained = true;
#endif
                    }
                }
                const bool waited = !wait_reg_mem->Test(regs.reg_array);
                if (vo_port->IsVoLabel(wait_addr) &&
                    num_tasks == mapped_queues[GfxQueueId].submits.size()) {
                    vo_port->WaitVoLabel([&] { return wait_reg_mem->Test(regs.reg_array); },
                                         report_wait);
                    if (waited) {
                        RecordFrontendWait(FrontendWait::GfxVoLabel,
                                           reinterpret_cast<uintptr_t>(wait_addr), wait_start);
                    }
                    break;
                }
                GpuWaitDiagnostics diagnostics;
#ifndef NDEBUG
                if (diag_drained && !waited) ++wait_drains[0].label_first; // DIAG-SGT-027
#endif
                if (waited && SatisfiedByPendingFence(rasterizer, wait_reg_mem)) {
#ifndef NDEBUG
                    ++wait_drains[0].fence_first; // DIAG-SGT-027
#endif
                    break;
                }
#ifndef NDEBUG
                if (waited) ++wait_drains[0].loops; // DIAG-SGT-027
#endif
                if (rasterizer && !wait_reg_mem->Test(regs.reg_array)) {
                    rasterizer->FlushForDeferredFences();
                }
                // Other queues run while this one yields and may record more work.
                // DIAG-047: a queue's job writing this label meanwhile is counted.
                waited_label = reinterpret_cast<VAddr>(wait_reg_mem->Address<u32*>());
#ifndef NDEBUG
                // DIAG-SGT-027: the same loop, counted (a drain and a fence check a turn).
                const auto diag_turn = [&] {
                    auto& tally = wait_drains[0];
                    ++tally.turns;
                    ++tally.loop_drains;
                    SyncRecorder("wait reg mem");
                    const bool fence = !wait_reg_mem->Test(regs.reg_array) &&
                                       SatisfiedByPendingFence(rasterizer, wait_reg_mem);
                    tally.loop_end_fence += fence;
                    return !fence;
                };
                while (!wait_reg_mem->Test(regs.reg_array) && diag_turn()) {
#else
                while (!wait_reg_mem->Test(regs.reg_array) &&
                       (SyncRecorder("wait reg mem"),
                        !SatisfiedByPendingFence(rasterizer, wait_reg_mem))) {
#endif
                    if (diagnostics.Ready()) {
                        report_wait();
                        if (rasterizer) {
                            rasterizer->FlushForDeferredFences();
                        }
                    }
                    YIELD_GFX();
                }
                waited_label = 0;
                if (waited) {
                    RecordFrontendWait(vo_port->IsVoLabel(wait_addr) ? FrontendWait::GfxVoLabel
                                                                     : FrontendWait::GfxWaitRegMem,
                                       reinterpret_cast<uintptr_t>(wait_addr), wait_start);
                }
                break;
            }
            case PM4ItOpcode::IndirectBuffer: {
                const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
                RecordCmdBuffer(indirect_buffer->Address<const u32>(),
                                u64(indirect_buffer->ib_size) * sizeof(u32), true);
                CheckIbAtDecode(indirect_buffer->Address<const u32>(), indirect_buffer->ib_size,
                                decoding_submit.load(std::memory_order_relaxed));
                lookahead_outer.push_back(lookahead_dcb);
                const auto outer_ib = CurrentDecodeIb(); // DIAG-063
                SetCurrentDecodeIb(indirect_buffer->Address<const u32>(), indirect_buffer->ib_size);
                auto task = ProcessGraphics(
                    {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, {});
                // PERF-GTS-383 (SHADGT_DIRECT_NESTED_RESUME=1): GT Sport calls each command
                // buffer from the one before it, over 250 deep in a race. A yield left every
                // one of them in turn and the next resume entered them all again: 47% of this
                // thread's CPU time in a 20-car race (it ran at 88% of a core). The thread's
                // loop now resumes the innermost buffer itself; this one is resumed when it
                // has finished.
                const bool direct_resume = DirectNestedResume();
                if (direct_resume) {
                    mapped_queues[GfxQueueId].nest.push_back(task.handle);
                }
                RESUME_GFX(task);

                while (!task.handle.done()) {
                    YIELD_GFX();
                    if (!direct_resume) {
                        RESUME_GFX(task);
                    }
                }
                if (direct_resume) {
                    ASSERT(mapped_queues[GfxQueueId].nest.back() == task.handle);
                    mapped_queues[GfxQueueId].nest.pop_back();
                }
                SetCurrentDecodeIb(outer_ib.first, outer_ib.second);
                // FIX-054: a finished coroutine's frame stays allocated until destroyed. GT Sport
                // calls thousands of indirect buffers a second, and their 3 KB frames filled the
                // heap (over 20 GB in a race, then "out of memory").
                task.handle.destroy();
                lookahead_outer.pop_back();
                break;
            }
            case PM4ItOpcode::IncrementDeCounter: {
                ++cblock.de_count;
                break;
            }
            case PM4ItOpcode::WaitOnCeCounter: {
                GpuWaitDiagnostics diagnostics;
                while (cblock.ce_count <= cblock.de_count && !ce_task.handle.done()) {
                    if (diagnostics.Ready()) {
                        LOG_WARNING(Render, "GPU DE wait stalled: CE={} DE={}", cblock.ce_count,
                                    cblock.de_count);
                    }
                    RESUME_GFX(ce_task);
                }
                break;
            }
            case PM4ItOpcode::PfpSyncMe: {
                break;
            }
            case PM4ItOpcode::StrmoutBufferUpdate: {
                const auto* strmout = reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(header);
                LOG_WARNING(Render_Vulkan,
                            "Unimplemented IT_STRMOUT_BUFFER_UPDATE, update_memory = {}, "
                            "source_select = {}, buffer_select = {}",
                            strmout->update_memory.Value(),
                            magic_enum::enum_name(strmout->source_select.Value()),
                            strmout->buffer_select.Value());
                break;
            }
            case PM4ItOpcode::GetLodStats: {
                LOG_WARNING(Render_Vulkan, "Unimplemented IT_GET_LOD_STATS");
                break;
            }
            case PM4ItOpcode::CondExec: {
                const auto* cond_exec = reinterpret_cast<const PM4CmdCondExec*>(header);
                if (cond_exec->command.Value() != 0) {
                    LOG_WARNING(Render, "IT_COND_EXEC used a reserved command");
                }
                // PERF-031: the condition may be written by recorded work.
                SyncRecorder("cond exec");
                const auto skip = *cond_exec->Address() == false;
                if (skip) {
                    dcb = NextPacket(dcb,
                                     header->type3.NumWords() + 1 + cond_exec->exec_count.Value());
                    continue;
                }
                break;
            }
            default:
                UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                                static_cast<u32>(opcode), count);
            }
            // A fence in this packet or a nested IB can let the guest recycle the buffer.
            // Its header may have changed while we yielded; advance using the captured count.
            dcb = NextPacket(dcb, count + 1);
            break;
        }
    }

    if (ce_task.handle) {
        while (!ce_task.handle.done()) {
            RESUME_GFX(ce_task);
        }
        ce_task.handle.destroy();
    }
    std::erase_if(live_cmd_buffers,
                  [&](const auto& span) { return span.data() == live_dcb.data(); });

    FIBER_EXIT;
}

bool Liverpool::ScanPackets(std::span<const u32> dcb,
                            const std::function<bool(const Regs&)>& on_draw, u32 depth,
                            u32& draws_left, u32& builds_left, const u32** stopped_at) {
    auto& shadow = *scan_regs;
    const auto set_regs = [&](u32 first, const u32* values, u32 num) {
        if (first + num <= shadow.reg_array.size()) {
            std::memcpy(&shadow.reg_array[first], values, num * sizeof(u32));
        }
    };
    while (!dcb.empty()) {
        if (draws_left == 0 || builds_left == 0) {
            if (stopped_at) {
                *stopped_at = dcb.data();
            }
            return false;
        }
        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        const u32 type = header->type;
        if (type == 0) {
            const auto write = DecodeType0RegisterWrite(dcb, shadow.reg_array.size());
            if (!write) {
                return true;
            }
            set_regs(write->first_register, write->values.data(),
                     static_cast<u32>(write->values.size()));
            dcb = dcb.subspan(std::min<size_t>(dcb.size(), write->values.size() + 1));
            continue;
        }
        if (type == 2) {
            dcb = dcb.subspan(1);
            continue;
        }
        if (type != 3) {
            // Not a command stream (or past its end); stop reading ahead.
            return true;
        }
        const u32 count = header->type3.NumWords();
        if (count + 1 > dcb.size()) {
            return true;
        }
        const auto* payload = reinterpret_cast<const u32*>(header + 2);
        const u32 num_values = count > 0 ? count - 1 : 0;
        switch (header->type3.opcode) {
        case PM4ItOpcode::ClearState:
            shadow.SetDefaults();
            break;
        case PM4ItOpcode::SetConfigReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            set_regs(Regs::ConfigRegWordOffset + set_data->reg_offset, payload, num_values);
            break;
        }
        case PM4ItOpcode::SetContextReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            set_regs(Regs::ContextRegWordOffset + set_data->reg_offset, payload, num_values);
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            // Compute shader registers are kept apart from the graphics state.
            if (set_data->reg_offset < 0x200 ||
                set_data->reg_offset > 0x200 + sizeof(ComputeProgram) / 4) {
                set_regs(Regs::ShRegWordOffset + set_data->reg_offset, payload, num_values);
            }
            break;
        }
        case PM4ItOpcode::SetUconfigReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            set_regs(Regs::UconfigRegWordOffset + set_data->reg_offset, payload, num_values);
            break;
        }
        case PM4ItOpcode::IndexType:
            shadow.index_buffer_type.raw =
                reinterpret_cast<const PM4CmdDrawIndexType*>(header)->raw;
            break;
        case PM4ItOpcode::NumInstances:
            shadow.num_instances.num_instances =
                reinterpret_cast<const PM4CmdDrawNumInstances*>(header)->num_instances;
            break;
        case PM4ItOpcode::DrawIndex2:
        case PM4ItOpcode::DrawIndexOffset2:
        case PM4ItOpcode::DrawIndexAuto:
        case PM4ItOpcode::DrawIndirect:
        case PM4ItOpcode::DrawIndirectMulti:
        case PM4ItOpcode::DrawIndexIndirect:
        case PM4ItOpcode::DrawIndexIndirectMulti:
        case PM4ItOpcode::DrawIndexIndirectCountMulti:
            if (!Common::PerfFeatureEnabled(22) || scanned_packets.insert(header).second) {
                --draws_left;
                if (on_draw(shadow)) {
                    --builds_left;
                }
            }
            break;
        case PM4ItOpcode::IndirectBuffer: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            const auto* ib = indirect_buffer->Address<const u32>();
            const u32 ib_size = indirect_buffer->ib_size;
            const bool new_ib =
                !Common::PerfFeatureEnabled(22) || scanned_packets.insert(ib).second;
            if (new_ib && depth < 4 && ib && ib_size > 0 && ib_size < (1u << 22)) {
                ScanPackets({ib, ib_size}, on_draw, depth + 1, draws_left, builds_left, nullptr);
                if (scan_blocked) {
                    // FIX-061: read it again once its commands are published.
                    scanned_packets.erase(ib);
                    return true;
                }
            }
            break;
        }
        case PM4ItOpcode::Rewind: {
            // FIX-061: the game writes the commands after a REWIND once it has submitted the
            // command buffer, and publishes them by setting the packet's valid bit; the decoder
            // waits for it. Until then they are an earlier frame's commands. GT Sport's race
            // summary submits each frame this way, and the read-ahead read the previous use of
            // the buffer: draws whose constants had been reused for other data, and hundreds of
            // pipelines nothing needed, which kept the driver figure's real pipelines waiting
            // (the figure flickered and glowed green while its draws were skipped).
            static const bool stop_at_rewind = Common::PerfFeatureEnabled(85);
            if (stop_at_rewind && count >= 1 &&
                (std::atomic_ref<const u32>(dcb[1]).load(std::memory_order_acquire) &
                 0x80000000u) == 0) {
                scan_blocked = true;
                ++pipe_read_ahead_stats.rewinds;
                if (stopped_at) {
                    *stopped_at = dcb.data(); // checked again by the next read-ahead
                    return false;
                }
                return true;
            }
            break;
        }
        default:
            break;
        }
        dcb = dcb.subspan(count + 1);
    }
    return true;
}

bool Liverpool::ScanAheadForPipelines(const std::function<bool(const Regs&)>& on_draw,
                                      u32 max_draws, u32 max_builds) {
    if (lookahead_dcb.empty()) {
        return false;
    }
    const u32* buffer_end = lookahead_dcb.data() + lookahead_dcb.size();
    std::span<const u32> start = lookahead_dcb;
    if (scan_regs && scan_buffer_end == buffer_end && scan_resume &&
        scan_resume >= lookahead_dcb.data() && scan_resume <= buffer_end) {
        // An earlier scan of this command buffer got past this point; continue it with the
        // registers it had there.
        if (scan_finished) {
            return false;
        }
        start = {scan_resume, buffer_end};
    } else {
        if (!scan_regs) {
            scan_regs = std::make_unique<Regs>();
            *scan_regs = regs;
        } else if (Common::PerfFeatureEnabled(23)) {
            // PERF-023: copy only the register blocks pipeline state comes from (config,
            // graphics shader, context and uconfig), about 40 KB instead of all 208 KB.
            const auto copy = [&](u32 first, u32 num) {
                std::memcpy(&scan_regs->reg_array[first], &regs.reg_array[first],
                            num * sizeof(u32));
            };
            copy(Regs::ConfigRegWordOffset, Regs::ShRegWordOffset - Regs::ConfigRegWordOffset);
            copy(Regs::ShRegWordOffset, 0x200);
            copy(Regs::ContextRegWordOffset, 0x400);
            copy(Regs::UconfigRegWordOffset, Regs::NumRegs - Regs::UconfigRegWordOffset);
        } else {
            *scan_regs = regs;
        }
        scan_buffer_end = buffer_end;
    }
    // Enough to keep every build worker busy, without reading a whole frame ahead each time.
    u32 draws_left = max_draws;
    u32 builds_left = max_builds;
    const u32* stopped_at = nullptr;
    scan_blocked = false;
    scan_finished = ScanPackets(start, on_draw, 0, draws_left, builds_left, &stopped_at);
    // The command buffers this one was called from continue after it.
    for (auto it = lookahead_outer.rbegin();
         scan_finished && !scan_blocked && it != lookahead_outer.rend(); ++it) {
        if (!it->empty()) {
            ScanPackets(*it, on_draw, 0, draws_left, builds_left, nullptr);
        }
    }
    scan_resume = scan_finished ? buffer_end : stopped_at;
    return !scan_finished;
}

struct ComputeParkedScope {
    u32& parked;
    explicit ComputeParkedScope(u32& value) : parked{value} { ++parked; }
    ~ComputeParkedScope() { --parked; }
};

template <bool is_indirect>
Liverpool::Task Liverpool::ProcessCompute(std::span<const u32> acb, u32 vqid) {
    FIBER_ENTER(acb_task_name[vqid]);
    auto& queue = asc_queues[{vqid}];

    struct IndirectPatch {
        const PM4Header* header;
        VAddr indirect_addr;
    };
    boost::container::small_vector<IndirectPatch, 4> indirect_patches;

    auto base_addr = reinterpret_cast<VAddr>(acb.data());
    size_t acb_size = acb.size_bytes();
    while (!acb.empty()) {
        ProcessCommands();

        auto* header = reinterpret_cast<const PM4Header*>(acb.data());
        u32 next_dw_off = header->type3.NumWords() + 1;

        // If we have a buffered packet, use it.
        if (queue.tmp_dwords > 0) [[unlikely]] {
            header = reinterpret_cast<const PM4Header*>(queue.tmp_packet.data());
            next_dw_off = header->type3.NumWords() + 1 - queue.tmp_dwords;
            std::memcpy(queue.tmp_packet.data() + queue.tmp_dwords, acb.data(),
                        next_dw_off * sizeof(u32));
            queue.tmp_dwords = 0;
        }

        // If the packet is split across ring boundary, buffer until next submission
        if (next_dw_off > acb.size()) [[unlikely]] {
            std::memcpy(queue.tmp_packet.data(), acb.data(), acb.size_bytes());
            queue.tmp_dwords = acb.size();
            if constexpr (!is_indirect) {
                *queue.read_addr += acb.size();
                *queue.read_addr %= queue.ring_size_dw;
            }
            break;
        }

        if (header->type == 2) {
            // Type-2 packet are used for padding purposes
            next_dw_off = 1;
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                *queue.read_addr += next_dw_off;
                *queue.read_addr %= queue.ring_size_dw;
            }
            continue;
        }

        if (header->type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", header->type.Value());
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        ++Common::GetWorkCounters().pm4_packets;

        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task = ProcessCompute<true>(
                {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, vqid);
            const bool direct_resume = DirectNestedResume(); // PERF-GTS-383
            if (direct_resume) {
                mapped_queues[vqid + 1].nest.push_back(task.handle);
            }
            RESUME_ASC(task, vqid);

            while (!task.handle.done()) {
                YIELD_ASC(vqid);
                if (!direct_resume) {
                    RESUME_ASC(task, vqid);
                }
            }
            if (direct_resume) {
                ASSERT(mapped_queues[vqid + 1].nest.back() == task.handle);
                mapped_queues[vqid + 1].nest.pop_back();
            }
            task.handle.destroy(); // FIX-054
            break;
        }
        case PM4ItOpcode::DmaData: {
            const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
            if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                break;
            }
            if (SkipRegisterSpaceDmaData(dma_data, "compute")) {
                break;
            }
            if (dma_data->dst_sel == DmaDataDst::Memory ||
                dma_data->dst_sel == DmaDataDst::MemoryUsingL2) {
                const bool src_memory = dma_data->src_sel == DmaDataSrc::Memory ||
                                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2;
                WatchWrite(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                           src_memory ? dma_data->SrcAddress<VAddr>() : 0, "compute DMA_DATA");
            }
            BumpPendingAscEpoch(); // PERF-095: this queue now reads memory
            // PERF-031: copies run on the recorder thread in order with the dispatches around them.
            if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
                RecordDma(
                    [this, dst = dma_data->dst_addr_lo, bytes = dma_data->NumBytes(),
                     value = dma_data->data] { rasterizer->FillBuffer(dst, bytes, value, true); });
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       dma_data->dst_sel == DmaDataDst::Gds) {
                RecordDma([this, dst = dma_data->dst_addr_lo, src = dma_data->SrcAddress<VAddr>(),
                        bytes = dma_data->NumBytes()] {
                    rasterizer->CopyBuffer(dst, src, bytes, true, false);
                }, dma_data->SrcAddress<VAddr>(), dma_data->NumBytes());
            } else if (dma_data->src_sel == DmaDataSrc::Data &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                RecordDma(
                    [this, dst = dma_data->DstAddress<VAddr>(), bytes = dma_data->NumBytes(),
                     value = dma_data->data] { rasterizer->FillBuffer(dst, bytes, value, false); },
                    0, 0, dma_data->DstAddress<VAddr>(), dma_data->NumBytes());
            } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                RecordDma([this, dst = dma_data->DstAddress<VAddr>(), src = dma_data->src_addr_lo,
                        bytes = dma_data->NumBytes()] {
                    rasterizer->CopyBuffer(dst, src, bytes, false, true);
                }, 0, 0, dma_data->DstAddress<VAddr>(), dma_data->NumBytes());
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                const u32 num_bytes = dma_data->NumBytes();
                const VAddr src_addr = dma_data->SrcAddress<VAddr>();
                const VAddr dst_addr = dma_data->DstAddress<VAddr>();
                const PM4Header* header =
                    reinterpret_cast<const PM4Header*>(dst_addr - sizeof(PM4Header));
                if (dst_addr >= base_addr && dst_addr < base_addr + acb_size &&
                    num_bytes == sizeof(PM4CmdDispatchIndirect::GroupDimensions) &&
                    header->type == 3 && header->type3.opcode == PM4ItOpcode::DispatchDirect) {
                    indirect_patches.emplace_back(header, src_addr);
                } else {
                    RecordDma([this, dst_addr, src_addr, num_bytes] {
                        rasterizer->CopyBuffer(dst_addr, src_addr, num_bytes, false, false);
                    }, src_addr, num_bytes, dst_addr, num_bytes);
                }
            } else {
                UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                                u32(dma_data->dst_sel));
            }
            break;
        }
        case PM4ItOpcode::AcquireMem: {
            break;
        }
        case PM4ItOpcode::Rewind: {
            if (!rasterizer) {
                break;
            }
            const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
            const auto wait_start = FrontendWaitNow();
            const bool waited = !rewind->Valid();
            ComputeParkedScope parked{mapped_queues[vqid + 1].parked};
            while (!rewind->Valid()) {
                YIELD_ASC(vqid);
            }
            if (waited) {
                RecordFrontendWait(FrontendWait::AscRewind, reinterpret_cast<uintptr_t>(header),
                                   wait_start);
            }
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto set_size = (header->type3.NumWords() - 1) * sizeof(u32);

            if (set_data->reg_offset >= 0x200 &&
                set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                ASSERT(set_size <= sizeof(ComputeProgram));
                auto* addr = reinterpret_cast<u32*>(&mapped_queues[vqid + 1].cs_state) +
                             (set_data->reg_offset - 0x200);
                std::memcpy(addr, header + 2, set_size);
            } else {
                std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                            header + 2, set_size);
                MarkRegs(Regs::ShRegWordOffset + set_data->reg_offset,
                         static_cast<u32>(set_size / sizeof(u32)));
            }
            break;
        }
        case PM4ItOpcode::SetQueueReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetQueueReg*>(header);
            LOG_WARNING(Render, "Encountered compute SetQueueReg: vqid = {}, reg_offset = {:#x}",
                        set_data->vqid.Value(), set_data->reg_offset.Value());
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            BumpPendingAscEpoch(); // PERF-095: this queue now reads memory
            if (auto it = std::ranges::find(indirect_patches, header, &IndirectPatch::header);
                it != indirect_patches.end()) {
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                RecordDraw([this, args = it->indirect_addr,
                            size] { rasterizer->DispatchIndirect(args, 0, size); },
                           true);
                break;
            }
            auto& cs_program = GetCsRegs();
            cs_program.dim_x = dispatch_direct->dim_x;
            cs_program.dim_y = dispatch_direct->dim_y;
            cs_program.dim_z = dispatch_direct->dim_z;
            cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            const auto cmd_address = reinterpret_cast<const void*>(header);
            RecordDraw(
                [this, vqid, cmd_address] {
                    rasterizer->ScopeMarker("asc[{}]:{}:DispatchDirect",
                                            fmt::make_format_args(vqid, cmd_address),
                                            [&] { rasterizer->DispatchDirect(); });
                },
                true, true);
            break;
        }
        case PM4ItOpcode::DispatchIndirect: {
            const auto* dispatch_indirect =
                reinterpret_cast<const PM4CmdDispatchIndirectMec*>(header);
            BumpPendingAscEpoch(); // PERF-095: this queue now reads memory
            auto& cs_program = GetCsRegs();
            const auto ib_address = dispatch_indirect->Address<VAddr>();
            const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            const auto cmd_address = reinterpret_cast<const void*>(header);
            RecordDraw(
                [this, vqid, cmd_address, ib_address, size] {
                    rasterizer->ScopeMarker(
                        "asc[{}]:{}:DispatchIndirect", fmt::make_format_args(vqid, cmd_address),
                        [&] { rasterizer->DispatchIndirect(ib_address, 0, size); });
                },
                true);
            break;
        }
        case PM4ItOpcode::WriteData: {
            const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
            ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
            const u32 data_size = (header->type3.count.Value() - 2) * 4;
            WatchWrite(write_data->Address<VAddr>(), data_size, 0, "compute WRITE_DATA");
            if (rasterizer) {
                if (auto* direct = rasterizer->GetBufferCache().GetDirectMemory()) {
                    direct->MarkVolatile(write_data->Address<VAddr>(), data_size); // D4
                }
            }
            if (!write_data->wr_one_addr.Value()) {
                std::vector<u8> data(data_size);
                std::memcpy(data.data(), write_data->data, data_size);
                std::vector<u8> label = data;
                if (TryEarlyLabel(write_data->Address<VAddr>(), data,
                                  u32(Vulkan::Rasterizer::DrainSource::AscWriteData))) {
                    break; // PERF-069: written at decode
                }
                RecordLabelWrite(
                    write_data->Address<VAddr>(), std::move(label),
                    [this, address = write_data->Address<VAddr>(), data = std::move(data)] {
#ifdef RYTY_GPU
                        // NAT-100: native mode writes it on the GPU in command order.
                        if (rasterizer && rasterizer->NativeInlineWrite(address, data)) {
                            return;
                        }
#endif
                        // DIAG-130 (SHADGT_DIRECT_HOLD_COMPUTE=1): with the direct renderer the
                        // label waits for the GPU like graphics labels do.
                        static const bool hold = [] {
                            const char* env = std::getenv("SHADGT_DIRECT_HOLD_COMPUTE");
                            return env && env[0] == '1';
                        }();
                        if (hold && rasterizer &&
                            rasterizer->GetBufferCache().GetDirectMemory() != nullptr) {
                            const std::vector<u8> value = data;
                            if (rasterizer->DeferFenceSignal(
                                    address,
                                    [address, data] {
                                        WriteDeferredFence(reinterpret_cast<void*>(address),
                                                           data.data(), u32(data.size()));
                                    },
                                    true, value, true)) {
                                return;
                            }
                        }
                        if (rasterizer) {
                            rasterizer->OnFence(Vulkan::Rasterizer::DrainSource::AscWriteData);
                        }
                        Core::MemoryManager::NoteEmulatorWrite(address, data.size(), data.data());
                        std::memcpy(reinterpret_cast<void*>(address), data.data(), data.size());
                        VideoCore::BumpUploadEpoch();
                    });
            } else {
                UNREACHABLE();
            }
            break;
        }
        case PM4ItOpcode::MemSemaphore: {
            const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
            if (mem_semaphore->IsSignaling()) {
                Record([semaphore = *mem_semaphore] { semaphore.Signal(); }, "memory semaphore");
            } else {
                const auto wait_start = FrontendWaitNow();
                if (!mem_semaphore->Signaled()) {
                    SyncRecorder("mem semaphore");
                }
                const bool waited = !mem_semaphore->Signaled();
                ComputeParkedScope parked{mapped_queues[vqid + 1].parked};
                while (!mem_semaphore->Signaled()) {
                    YIELD_ASC(vqid);
                }
                if (waited) {
                    RecordFrontendWait(FrontendWait::AscMemSemaphore,
                                       mem_semaphore->Address<uintptr_t>(), wait_start);
                }
                mem_semaphore->Decrement();
            }
            break;
        }
        case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            const auto wait_start = FrontendWaitNow();
            ComputeParkedScope parked{mapped_queues[vqid + 1].parked};
            // PERF-048: as for graphics waits, the other queues decode first.
            bool yielded = false;
            for (u32 turn = 0; draw_pipe && WaitTurnsEnabled() && turn < 4 &&
                               !wait_reg_mem->Test(regs.reg_array) &&
                               !PendingLabelSatisfies(*wait_reg_mem);
                 ++turn) {
                yielded = true;
                YIELD_ASC(vqid);
            }
            if (!wait_reg_mem->Test(regs.reg_array) && PendingLabelSatisfies(*wait_reg_mem)) {
                // PERF-031: a queued fence writes the label; the recorder waits for it in order.
                ++waits_moved;
                waits_moved_after_turns += yielded;
                RecordSafe([this, wait = *wait_reg_mem] { RecorderWaitRegMem(wait); });
                break;
            }
            // PERF-GT7-046: until the recorder has run dry, other queues decode.
#ifndef NDEBUG
            const auto diag_idle_from = std::chrono::steady_clock::now(); // DIAG-SGT-027
#endif
            while (WaitsYieldToRecorder() && !wait_reg_mem->Test(regs.reg_array) &&
                   !draw_pipe->QueueIdle()) {
                YIELD_ASC(vqid);
            }
#ifndef NDEBUG
            auto& tally = wait_drains[1]; // DIAG-SGT-027
            tally.idle_wait_us += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - diag_idle_from).count();
#endif
            // PERF-SGT-028 (SHADGT_WAIT_DRAIN_ON_LABEL=0: every turn, as before): a looping wait
            // drained the recorder on every turn, about 4,000 times a second in a GT7 race, and
            // each drain held this thread until the recorder caught up with what the other
            // queues had decoded meanwhile. A recorded fence only ends the wait once a write of
            // the label has been decoded (every deferred fence starts as RecordLabelWrite), and in
            // 1.1 million turns none ended one without such a write since the last drain
            // (DIAG-SGT-027). The loop drains on those turns, and on every 64th turn still.
            static const bool drain_on_label = [] {
                const char* env = std::getenv("SHADGT_WAIT_DRAIN_ON_LABEL");
                return !(env && env[0] == '0');
            }();
            const size_t slot = std::min<size_t>(vqid + 1, waited_labels.size() - 1);
            waited_labels[slot] = reinterpret_cast<VAddr>(wait_reg_mem->Address<u32*>());
            label_writes_over_wait[slot] = 0;
            ++waiting_queues;
            u64 label_writes_at_drain = 0;
            u32 skipped = 0;
            // PERF-031: what is waited on may be written by recorded work; the pending-fence
            // check reads the rasterizer, which only the drained recorder lets the command use.
            const auto unmet = [&](bool first) {
                if (wait_reg_mem->Test(regs.reg_array)) {
                    return false;
                }
                if (!first && drain_on_label &&
                    label_writes_over_wait[slot] == label_writes_at_drain && ++skipped % 64 != 0) {
                    // Nothing can have changed what the fence check says. The drain held this
                    // thread while the recorder worked; a skipped turn pauses instead.
                    if (skipped % 256 == 255) {
                        std::this_thread::yield();
                    } else {
                        _mm_pause();
                    }
                    return true;
                }
                // PERF-SGT-078: the first check, and a turn after a label was written over the
                // wait, without the drain; every 64th turn still drains (work that writes the
                // label without a label write).
                if (WaitSkipsFirstDrain() &&
                    (first || label_writes_over_wait[slot] != label_writes_at_drain)) {
                    label_writes_at_drain = label_writes_over_wait[slot];
                    if (PendingLabelSatisfies(*wait_reg_mem)) {
                        // As above: the recorder waits for it in order.
                        ++waits_moved;
                        RecordSafe([this, wait = *wait_reg_mem] { RecorderWaitRegMem(wait); });
                        return false;
                    }
                    if (SatisfiedByPendingFence(rasterizer, wait_reg_mem) ||
                        wait_reg_mem->Test(regs.reg_array)) {
                        return false;
                    }
                    if constexpr (Common::DiagnosticsBuilt) {
                        ::Vulkan::NoteSubmitSite(first ? "compute wait without drain"
                                                       : "compute wait turn without drain", {}); // DIAG-SGT-076
                    }
                    return true;
                }
#ifndef NDEBUG
                if (first) {
                    ++tally.packets;
                } else {
                    ++tally.loop_drains;
                    tally.rechecks += label_writes_over_wait[slot] == label_writes_at_drain;
                }
#endif
                label_writes_at_drain = label_writes_over_wait[slot];
                if constexpr (Common::DiagnosticsBuilt) {
                    ::Vulkan::NoteSubmitSite(first ? "compute wait first drain" : "compute wait loop drain", {});
                }
                SyncRecorder("wait reg mem");
                const bool label = wait_reg_mem->Test(regs.reg_array);
                const bool fence = !label && SatisfiedByPendingFence(rasterizer, wait_reg_mem);
#ifndef NDEBUG
                if (first) {
                    tally.label_first += label;
                    tally.fence_first += fence;
                    tally.loops += !label && !fence;
                } else {
                    tally.loop_end_fence += fence;
                }
#endif
                return !label && !fence;
            };
            const bool waited = unmet(true);
            while (waited && unmet(false)) {
#ifndef NDEBUG
                ++tally.turns;
#endif
                YIELD_ASC(vqid);
            }
#ifndef NDEBUG
            if (waited) {
                tally.loop_end_label += wait_reg_mem->Test(regs.reg_array);
            }
#endif
            waited_labels[slot] = 0;
            --waiting_queues;
            if (waited) {
                RecordFrontendWait(FrontendWait::AscWaitRegMem,
                                   reinterpret_cast<uintptr_t>(wait_reg_mem->Address<u64*>()),
                                   wait_start);
            }
            break;
        }
        case PM4ItOpcode::ReleaseMem: {
            // Signaling the fence can allow the guest to reuse the containing command buffer.
            const auto release = *reinterpret_cast<const PM4CmdReleaseMem*>(header);
            // PERF-010: when GPU-written pages the CPU read before are being read back, write
            // this fence once the GPU has executed the work and the data is in guest memory,
            // instead of letting the CPU access drain the GPU.
            const auto pipe_id = queue.pipe_id;
            if (release.data_sel != DataSelect::GdsMemStore &&
                release.data_sel != DataSelect::None) {
                const auto bytes = FenceValueBytes(release.data_sel.Value(), release.DataDWord(),
                                                   release.DataQWord());
                if (TryEarlyLabel(release.Address<VAddr>(), bytes,
                                  u32(Vulkan::Rasterizer::DrainSource::AscReleaseMem),
                                  release.int_sel != InterruptSelect::None ? int(pipe_id) : -1)) {
                    break; // PERF-069: signaled at decode
                }
            }
            // PERF-031: signaled in order behind the recorded work.
            RecordLabelWrite(
                release.data_sel != DataSelect::GdsMemStore ? release.Address<VAddr>() : 0,
                FenceValueBytes(release.data_sel.Value(), release.DataDWord(), release.DataQWord()),
                [this, release, pipe_id] {
                    const auto* release_mem = &release;
                    const bool deferred =
                        rasterizer && release.data_sel != DataSelect::GdsMemStore &&
                        rasterizer->DeferFenceSignal(
                            release.Address<VAddr>(),
                            [release, pipe_id] {
                                u64 value{};
                                u32 num_bytes = sizeof(u64);
                                switch (release.data_sel.Value()) {
                                case DataSelect::None:
                                    num_bytes = 0;
                                    break;
                                case DataSelect::Data32Low:
                                    value = release.DataDWord();
                                    num_bytes = sizeof(u32);
                                    break;
                                case DataSelect::Data64:
                                    value = release.DataQWord();
                                    break;
                                case DataSelect::GpuClock64:
                                    value = GetGpuClock64();
                                    break;
                                case DataSelect::PerfCounter:
                                    value = GetGpuPerfCounter();
                                    break;
                                default:
                                    UNREACHABLE();
                                }
                                if (num_bytes != 0) {
                                    WriteDeferredFence(release.Address<void*>(), &value, num_bytes);
                                }
                                if (release.int_sel != InterruptSelect::None) {
                                    Platform::IrqC::Instance()->Signal(
                                        static_cast<Platform::InterruptId>(pipe_id));
                                }
                            },
                            true,
                            FenceValueBytes(release.data_sel.Value(), release.DataDWord(),
                                            release.DataQWord()));
                    if (deferred) {
                        return;
                    }
                    if (rasterizer) {
                        rasterizer->OnFence(Vulkan::Rasterizer::DrainSource::AscReleaseMem);
                    }
                    // PERF-051: the value goes to guest memory through the backing, as graphics
                    // EOP/EOS fences do, instead of a store that faults on a tracked page and
                    // makes the recorder wait for the whole GPU. -DisablePerf 63 stores directly.
                    static const bool write_backing = Common::PerfFeatureEnabled(63);
                    release_mem->SignalFence(
                        [pipe_id] {
                            Platform::IrqC::Instance()->Signal(
                                static_cast<Platform::InterruptId>(pipe_id));
                        },
                        [this](VAddr dst, u16 gds_index, u16 num_dwords) {
                            rasterizer->CopyBuffer(dst, gds_index, num_dwords * sizeof(u32), false,
                                                   true);
                        },
                        [](void* address, u64 data, u32 num_bytes) {
                            auto* memory = Core::Memory::Instance();
                            if (!write_backing ||
                                !memory->TryWriteBacking(address, &data, num_bytes)) {
                                std::memcpy(address, &data, num_bytes);
                            }
                        });
                });
            break;
        }
        case PM4ItOpcode::EventWrite: {
            // const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
            break;
        }
        default:
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), header->type3.NumWords());
        }

        acb = NextPacket(acb, next_dw_off);

        if constexpr (!is_indirect) {
            *queue.read_addr += next_dw_off;
            *queue.read_addr %= queue.ring_size_dw;
        }
    }

    FIBER_EXIT;
}

Liverpool::CmdBuffer Liverpool::CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];
    ASSERT_MSG(queue.dcb_buffer.capacity() >= queue.dcb_buffer_offset + dcb.size(),
               "dcb copy buffer out of reserved space");
    ASSERT_MSG(queue.ccb_buffer.capacity() >= queue.ccb_buffer_offset + ccb.size(),
               "ccb copy buffer out of reserved space");

    queue.dcb_buffer.resize(
        std::max(queue.dcb_buffer.size(), queue.dcb_buffer_offset + dcb.size()));
    queue.ccb_buffer.resize(
        std::max(queue.ccb_buffer.size(), queue.ccb_buffer_offset + ccb.size()));

    const u32 prev_dcb_buffer_offset = queue.dcb_buffer_offset;
    const u32 prev_ccb_buffer_offset = queue.ccb_buffer_offset;
    if (!dcb.empty()) {
        std::memcpy(queue.dcb_buffer.data() + queue.dcb_buffer_offset, dcb.data(),
                    dcb.size_bytes());
        queue.dcb_buffer_offset += dcb.size();
        dcb = std::span<const u32>{queue.dcb_buffer.begin() + prev_dcb_buffer_offset,
                                   queue.dcb_buffer.begin() + queue.dcb_buffer_offset};
    }

    if (!ccb.empty()) {
        std::memcpy(queue.ccb_buffer.data() + queue.ccb_buffer_offset, ccb.data(),
                    ccb.size_bytes());
        queue.ccb_buffer_offset += ccb.size();
        ccb = std::span<const u32>{queue.ccb_buffer.begin() + prev_ccb_buffer_offset,
                                   queue.ccb_buffer.begin() + queue.ccb_buffer_offset};
    }

    return std::make_pair(dcb, ccb);
}

// DIAG-061: GT Sport's race summary sometimes finds its tessellation constants replaced by float
// data when the draw is decoded (FIX-058). This records each draw's constants as the game left them
// at submission, so a decoded draw can tell whether they were overwritten afterwards.
namespace {
std::mutex tess_submit_mutex;
std::unordered_map<VAddr, Liverpool::TessSubmitRecord> tess_submits;
u64 tess_submit_sane{};
u64 tess_submit_garbage{};
s64 tess_submit_last_log{};
struct {
    u64 packets, sh_sets, hs_touches, rejected, ibs, ibs_refused, bailed;
    u32 rejected_vsharp[4];
} tess_scan{};

// DIAG-062: indirect buffers as they were at submission, to tell whether the game still writes
// them after submitting.
struct IbAtSubmit {
    u64 hash{};
    u32 dwords{};
    u64 submit{};
    s64 time_ns{};
    std::vector<u32> words; // small buffers only
};
std::mutex ib_submit_mutex;
std::unordered_map<VAddr, IbAtSubmit> ib_at_submit;
std::atomic<bool> current_ib_changed{};
u64 ibs_unchanged{};
u64 ibs_changed{};
u64 ibs_unseen{};

bool TessSubmitCheckEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADGT_TESS_SUBMIT_CHECK");
        return env && *env && *env != '0';
    }();
    return enabled;
}

constexpr u32 HsUserDataFirst = 0x2D0C; // SPI_SHADER_USER_DATA_HS_0
constexpr u32 TessConstsDword = 7;      // where GT Sport's hull shaders read the V# from

void CheckTessConstantsAtSubmit(const std::array<u32, 16>& hs, u64 submit, s64 now,
                                std::span<const u32> cmd) {
    const u32* v = &hs[TessConstsDword];
    ++tess_scan.hs_touches;
    if (((v[1] >> 16) & 0x3fff) != 16 || v[2] != 3) {
        ++tess_scan.rejected;
        std::memcpy(tess_scan.rejected_vsharp, v, sizeof(tess_scan.rejected_vsharp));
        return;
    }
    const VAddr base = (VAddr(v[1] & 0xffff) << 32) | v[0];
    std::array<u32, 7> c{};
#ifdef RYTY_GPU
    if (!Core::RytyMemory::IsGuestReadable(base, sizeof(c))) {
        return;
    }
#endif
    Common::CopyGuest(c.data(), reinterpret_cast<const void*>(base), sizeof(c));
    constexpr u32 Lds = 64_KB;
    const bool sane = c[2] != 0 && std::ranges::all_of(c, [](u32 x) { return x <= Lds; });
    const VAddr cmd_begin = reinterpret_cast<VAddr>(cmd.data());
    const bool in_cmd = base >= cmd_begin && base < cmd_begin + cmd.size_bytes();
    std::scoped_lock lk{tess_submit_mutex};
    if (tess_submits.size() > 1'000'000) {
        tess_submits.clear();
    }
    tess_submits[base] = {submit, now, c[0], sane, in_cmd};
    ++(sane ? tess_submit_sane : tess_submit_garbage);
    if (!sane && tess_submit_garbage <= 40) {
        LOG_WARNING(Render, "DIAG-061: submission {} carries garbage tessellation constants at "
                            "{:#x} already (ls stride {:#x})",
                    submit, base, c[0]);
    }
    if (now - tess_submit_last_log >= 5'000'000'000) {
        tess_submit_last_log = now;
        LOG_WARNING(Render, "DIAG-061: tessellation constants at submission: {} sane, {} garbage",
                    tess_submit_sane, tess_submit_garbage);
    }
}

void ScanTessConstants(std::span<const u32> cmd, std::array<u32, 16>& hs, u64 submit, s64 now,
                       u32 depth) {
    auto apply = [&](u32 first, std::span<const u32> values) {
        bool touched = false;
        for (u32 i = 0; i < values.size(); ++i) {
            const u32 reg = first + i;
            if (reg >= HsUserDataFirst && reg < HsUserDataFirst + 16) {
                hs[reg - HsUserDataFirst] = values[i];
                touched |= reg - HsUserDataFirst >= TessConstsDword &&
                           reg - HsUserDataFirst < TessConstsDword + 4;
            }
        }
        if (touched) {
            CheckTessConstantsAtSubmit(hs, submit, now, cmd);
        }
    };
    std::span<const u32> rest = cmd;
    while (!rest.empty()) {
        const u32 raw = rest.front();
        const u32 type = raw >> 30;
        if (type == 2) {
            rest = rest.subspan(1);
            continue;
        }
        if (type == 0) {
            const auto write = DecodeType0RegisterWrite(rest, Regs::NumRegs);
            if (!write) {
                ++tess_scan.bailed;
                return;
            }
            apply(write->first_register, write->values);
            rest = rest.subspan(write->values.size() + 1);
            continue;
        }
        if (type != 3) {
            ++tess_scan.bailed;
            return;
        }
        const auto* header = reinterpret_cast<const PM4Type3Header*>(rest.data());
        const u32 count = header->NumWords();
        if (count + 1 > rest.size()) {
            ++tess_scan.bailed;
            return;
        }
        ++tess_scan.packets;
        if (header->opcode == PM4ItOpcode::SetShReg && count >= 1) {
            ++tess_scan.sh_sets;
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            apply(Regs::ShRegWordOffset + set_data->reg_offset, rest.subspan(2, count - 1));
        } else if (header->opcode == PM4ItOpcode::IndirectBuffer && depth < 4) {
            const auto* ib = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            const auto* data = ib->Address<const u32>();
            const u64 size = u64(ib->ib_size) * sizeof(u32);
#ifdef RYTY_GPU
            if (data && Core::RytyMemory::IsGuestReadable(reinterpret_cast<VAddr>(data), size))
#else
            if (data)
#endif
            {
                ++tess_scan.ibs;
                {
                    const u64 hash = XXH3_64bits(data, size);
                    std::scoped_lock lk{ib_submit_mutex};
                    if (ib_at_submit.size() > 200'000) {
                        ib_at_submit.clear();
                    }
                    auto& entry = ib_at_submit[reinterpret_cast<VAddr>(data)];
                    entry = {hash, ib->ib_size, submit, now, {}};
                    if (ib->ib_size <= 64) {
                        entry.words.assign(data, data + ib->ib_size);
                    }
                }
                ScanTessConstants({data, ib->ib_size}, hs, submit, now, depth + 1);
            } else {
                ++tess_scan.ibs_refused;
            }
        }
        rest = rest.subspan(count + 1);
    }
}
} // namespace

namespace {
std::atomic<const u32*> current_decode_ib{};
std::atomic<u32> current_decode_ib_dwords{};
} // namespace

std::pair<const u32*, u32> Liverpool::CurrentDecodeIb() {
    return {current_decode_ib.load(std::memory_order_relaxed),
            current_decode_ib_dwords.load(std::memory_order_relaxed)};
}

void Liverpool::SetCurrentDecodeIb(const u32* data, u32 dwords) {
    current_decode_ib.store(data, std::memory_order_relaxed);
    current_decode_ib_dwords.store(dwords, std::memory_order_relaxed);
}

std::string Liverpool::DescribeEops() {
    const s64 last = g_last_eop_ns.load();
    return fmt::format("{} EOPs signaled, the last one of submission {} {:.2f} ms ago",
                       g_eop_count.load(), g_eop_submit.load(),
                       last ? (NowNs() - last) / 1e6 : -1.0);
}

bool Liverpool::CurrentIbChangedSinceSubmit() {
    return current_ib_changed.load(std::memory_order_relaxed);
}

// DIAG-062: compares an indirect buffer being decoded with its contents at submission.
static void CheckIbAtDecode(const u32* data, u32 dwords, u64 decoding) {
    if (!TessSubmitCheckEnabled()) {
        return;
    }
    const u64 hash = XXH3_64bits(data, u64(dwords) * sizeof(u32));
    std::scoped_lock lk{ib_submit_mutex};
    const auto it = ib_at_submit.find(reinterpret_cast<VAddr>(data));
    bool changed = false;
    if (it == ib_at_submit.end() || it->second.dwords != dwords) {
        ++ibs_unseen;
    } else if (it->second.hash == hash) {
        ++ibs_unchanged;
    } else {
        changed = true;
        if (++ibs_changed <= 6 && !it->second.words.empty()) {
            LOG_WARNING(Render, "DIAG-062: indirect buffer {:#x} at submission: {:08x}",
                        reinterpret_cast<VAddr>(data), fmt::join(it->second.words, " "));
            LOG_WARNING(Render, "DIAG-062: indirect buffer {:#x} at decoding:   {:08x}",
                        reinterpret_cast<VAddr>(data),
                        fmt::join(std::span<const u32>{data, dwords}, " "));
        }
        if (ibs_changed <= 40) {
            LOG_WARNING(Render,
                        "DIAG-062: indirect buffer {:#x} ({} dwords) of submission {} changed "
                        "between submission and decoding ({:.2f} ms later, decoding submission "
                        "{})",
                        reinterpret_cast<VAddr>(data), dwords, it->second.submit,
                        (NowNs() - it->second.time_ns) / 1e6, decoding);
        }
    }
    current_ib_changed.store(changed, std::memory_order_relaxed);
    static s64 last_log = 0;
    if (const s64 now = NowNs(); now - last_log >= 5'000'000'000) {
        last_log = now;
        LOG_WARNING(Render,
                    "DIAG-062: indirect buffers at decoding: {} as submitted, {} changed since "
                    "submission, {} not seen at submission",
                    ibs_unchanged, ibs_changed, ibs_unseen);
    }
}

std::optional<Liverpool::TessSubmitRecord> Liverpool::FindTessSubmit(VAddr base) {
    std::scoped_lock lk{tess_submit_mutex};
    if (const auto it = tess_submits.find(base); it != tess_submits.end()) {
        return it->second;
    }
    return std::nullopt;
}

void Liverpool::SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];
    const auto original_dcb = dcb;
    if (!dcb.empty()) {
        const u64 submit = submitted_count.fetch_add(1, std::memory_order_relaxed) + 1;
        NoteSubmitForLatency(submit); // DIAG-SGT-074
        if (TessSubmitCheckEnabled()) {
            static std::array<u32, 16> hs{}; // only this thread submits graphics work
            const s64 now = NowNs();
            if (submit % 2000 == 0 || submit % 2000 == 1) {
                // DIAG-062: what a submission consists of.
                std::string packets;
                u32 listed = 0;
                for (auto rest = dcb; !rest.empty() && listed < 80; ++listed) {
                    const u32 raw = rest.front();
                    const u32 type = raw >> 30;
                    size_t words = 1;
                    if (type == 3) {
                        const auto* h = reinterpret_cast<const PM4Type3Header*>(rest.data());
                        words = h->NumWords() + 1;
                        packets += fmt::format(" {:02x}/{}", u32(h->opcode.Value()), words);
                        if (h->opcode == PM4ItOpcode::IndirectBuffer) {
                            const auto* ib = reinterpret_cast<const PM4CmdIndirectBuffer*>(h);
                            packets += fmt::format("->{:#x}+{}", ib->Address<const u32>()
                                                                  ? reinterpret_cast<VAddr>(
                                                                        ib->Address<const u32>())
                                                                  : 0,
                                                   u32(ib->ib_size));
                        } else if (h->opcode == PM4ItOpcode::WaitRegMem) {
                            const auto* w = reinterpret_cast<const PM4CmdWaitRegMem*>(h);
                            packets += fmt::format("(addr {:#x} ref {:#x} mask {:#x} fn {})",
                                                   reinterpret_cast<VAddr>(w->Address<u32*>()),
                                                   u32(w->ref), u32(w->mask),
                                                   u32(w->function.Value()));
                        }
                    } else if (type == 0) {
                        words = ((raw >> 16) & 0x3fff) + 2;
                        packets += fmt::format(" t0:{:#x}/{}", raw & 0xffff, words);
                    } else {
                        packets += " t2";
                    }
                    if (words > rest.size()) {
                        break;
                    }
                    rest = rest.subspan(words);
                }
                LOG_WARNING(Render, "DIAG-062: submission {} at {:#x}, {} dwords:{}", submit,
                            reinterpret_cast<VAddr>(dcb.data()), dcb.size(), packets);
            }
            ScanTessConstants(dcb, hs, submit, now, 0);
            static s64 last_scan_log = 0;
            if (now - last_scan_log >= 5'000'000'000) {
                last_scan_log = now;
                const auto& t = tess_scan;
                LOG_WARNING(Render,
                            "DIAG-061: scanned {} packets, {} SET_SH_REG, {} HS V# writes ({} not "
                            "tessellation V#s, last {:08x} {:08x} {:08x} {:08x}), {} IBs ({} "
                            "unreadable), {} scans stopped early",
                            t.packets, t.sh_sets, t.hs_touches, t.rejected, t.rejected_vsharp[0],
                            t.rejected_vsharp[1], t.rejected_vsharp[2], t.rejected_vsharp[3],
                            t.ibs, t.ibs_refused, t.bailed);
            }
        }
    }

    if (EmulatorSettings.IsCopyGpuBuffers()) {
        std::tie(dcb, ccb) = CopyCmdBuffers(dcb, ccb);
    }

    // PERF-012: memory written before this submission must be uploaded again.
    VideoCore::BumpUploadEpoch();
    VideoCore::g_gfx_submit_count.fetch_add(1, std::memory_order_release); // EXP-126
#ifndef NDEBUG
    {
        // DIAG-051
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lk{submit_timing_mutex};
        gfx_submit_times.push_back(now);
        if constexpr (Common::Nvtx::Enabled) {
            const u64 n = ++gfx_submit_number;
            gfx_submit_ranges.push_back(
                {n, Common::Nvtx::Start(fmt::format("gfx submit {}: decode", n)),
                 Common::Nvtx::Start(fmt::format("gfx submit {}: until recorded", n))});
        }
        if (const s64 eop = g_last_eop_ns.load(); eop != 0) {
            const s64 since = NowNs() - eop;
            if (since >= 0 && since < 100'000'000) {
                submit_stats.eop_to_submit_us += static_cast<u64>(since / 1000);
                ++submit_stats.eop_to_submit_count;
            }
        }
        if (last_gfx_submit != std::chrono::steady_clock::time_point{}) {
            submit_stats.gap_us += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - last_gfx_submit)
                    .count());
            ++submit_stats.gaps;
        }
        last_gfx_submit = now;
    }
#endif
    auto task = ProcessGraphics(dcb, ccb, original_dcb);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    ++num_submits;
    ++num_tasks;
    submit_cv.notify_one();
}

void Liverpool::SubmitAsc(u32 gnm_vqid, std::span<const u32> acb) {
    ASSERT_MSG(gnm_vqid > 0 && gnm_vqid < NumTotalQueues, "Invalid virtual ASC queue index");
    auto& queue = mapped_queues[gnm_vqid];

    const auto vqid = gnm_vqid - 1;
    NoteAscEpoch(); // PERF-095
    const auto& task = ProcessCompute(acb, vqid);
    {
        std::scoped_lock lock{queue.m_access};
        queue.pending.fetch_add(1, std::memory_order_relaxed);
        queue.submits.emplace(task.handle);
        queue.pending.fetch_add(1, std::memory_order_relaxed);
    }

    std::scoped_lock lk{submit_mutex};
    num_mapped_queues.store(std::max(num_mapped_queues.load(std::memory_order_relaxed),
                                     gnm_vqid + 1), std::memory_order_release);
    ++num_submits;
    ++num_tasks;
    submit_cv.notify_one();
}

} // namespace AmdGpu
