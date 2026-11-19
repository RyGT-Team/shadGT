// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// DIAG-SGT-023 (SHADGT_DIAG_ORDERED_FRACTION=1): how much of the recorder's time per draw would
// have to run in draw order on a two-thread recorder (FEAT-SGT-022), and how much could run on
// both threads at once. A Scope at the top of a function attributes its time to that site.
// Time is exclusive: a scope's own time minus the scopes it encloses. Its kind is inherited:
// inside an ordered or barrier scope everything counts as ordered or barrier, whatever the
// child is. What remains of a draw's or dispatch's total once the tagged scopes are taken out
// is the parallel candidate. Off: one predictable branch per scope.

#include <array>
#include <cstdint>
#include <cstdlib>

#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64)
#include <intrin.h>
#endif

namespace Native::OrderedTiming {

enum class Kind : std::uint8_t {
    Parallel,  ///< a draw or dispatch total: what tagged scopes leave is order-free
    Uncertain, ///< could become order-free with work (pipeline lookup, registry find, targets)
    Ordered,   ///< changes shared state or consumes pending state: needs the ticket turn
    Barrier,   ///< needs both recorder threads drained (splice, flip, fills, unmaps)
    Count,
};

enum Site : std::uint8_t {
    DrawTotal,
    DispatchTotal,
    Begin,
    Targets,
    MakeRegion,
    RefreshRegion,
    SortRegionWrites,
    MirrorBuffer,
    RefreshMirror,
    MakePendingMirrors,
    DropMirrors,
    PerFrameDrops,
    GatherOnGpu,
    SnapshotRange,
    Import,
    PrepareImages,
    WritebacksBefore,
    RegionPlan,
    GlobalBarrier,
    FlushWritebacks,
    RefreshSources,
    ApplyRewrite,
    RegistryFind,
    MakeImage,
    NoteWrites,
    CommitPrepared,
    LongSetsUpkeep,
    PipelineLookup,
    Splice,
    FindDisplay,
    FillCopyWrite,
    Mapping,
    SiteCount,
};

inline constexpr std::array<Kind, SiteCount> kKind{
    Kind::Parallel,  Kind::Parallel,  Kind::Ordered,   Kind::Uncertain, Kind::Ordered,
    Kind::Ordered,   Kind::Ordered,   Kind::Uncertain, Kind::Ordered,   Kind::Ordered,
    Kind::Ordered,   Kind::Ordered,   Kind::Ordered,   Kind::Ordered,   Kind::Uncertain,
    Kind::Ordered,   Kind::Ordered,   Kind::Ordered,   Kind::Ordered,   Kind::Ordered,
    Kind::Ordered,   Kind::Ordered,   Kind::Uncertain, Kind::Ordered,   Kind::Ordered,
    Kind::Ordered,   Kind::Ordered,   Kind::Uncertain, Kind::Barrier,   Kind::Barrier,
    Kind::Barrier,   Kind::Barrier,
};

inline constexpr std::array<const char*, SiteCount> kName{
    "draw",           "dispatch",       "Begin",           "ResolveTargets",
    "MakeRegion",     "RefreshRegion",  "SortRegionWrites", "MirrorBuffer",
    "RefreshMirror",  "MakePendingMirrors", "DropMirrors", "PerFrameDrops",
    "GatherOnGpu",    "SnapshotRange",  "Import",          "PrepareImages",
    "WritebacksBefore", "RegionPlan",   "GlobalBarrier",   "FlushWritebacks",
    "RefreshSources", "ApplyRewrite",   "RegistryFind",    "MakeImage",
    "NoteWrites",     "CommitPrepared", "LongSetsUpkeep",  "PipelineLookup",
    "Splice",         "FindDisplay",    "Fill/Copy/InlineWrite", "Unmap/NewMapping",
};

inline bool Enabled() {
    static const bool on = [] {
        const char* env = std::getenv("SHADGT_DIAG_ORDERED_FRACTION");
        return env && env[0] == '1';
    }();
    return on;
}

inline std::uint64_t Ticks() {
    return __rdtsc();
}

/// One thread's accumulated ticks, read and reset by that thread's report.
struct Totals {
    std::array<std::uint64_t, SiteCount> site{};
    std::array<std::uint64_t, static_cast<std::size_t>(Kind::Count)> kind{};
    std::uint64_t draws = 0;
    std::uint64_t dispatches = 0;
};

inline Totals& Local() {
    static thread_local Totals totals;
    return totals;
}

struct StackFrame {
    std::uint64_t start;
    std::uint64_t children;
    Site site;
    Kind kind; ///< effective: inherited from an ordered or barrier parent
};

struct Stack {
    std::array<StackFrame, 64> frames;
    std::uint32_t depth = 0;
};

inline Stack& LocalStack() {
    static thread_local Stack stack;
    return stack;
}

class Scope {
public:
    explicit Scope(Site site) {
        if (!Enabled()) {
            return;
        }
        Stack& stack = LocalStack();
        if (stack.depth >= stack.frames.size()) {
            return;
        }
        Kind kind = kKind[site];
        if (stack.depth != 0) {
            const Kind parent = stack.frames[stack.depth - 1].kind;
            if (parent == Kind::Ordered || parent == Kind::Barrier) {
                kind = parent;
            }
        }
        stack.frames[stack.depth++] = {Ticks(), 0, site, kind};
        active = true;
    }
    ~Scope() {
        if (!active) {
            return;
        }
        Stack& stack = LocalStack();
        const StackFrame frame = stack.frames[--stack.depth];
        const std::uint64_t elapsed = Ticks() - frame.start;
        const std::uint64_t own = elapsed > frame.children ? elapsed - frame.children : 0;
        Totals& totals = Local();
        totals.site[frame.site] += own;
        totals.kind[static_cast<std::size_t>(frame.kind)] += own;
        if (frame.site == DrawTotal) {
            ++totals.draws;
        } else if (frame.site == DispatchTotal) {
            ++totals.dispatches;
        }
        if (stack.depth != 0) {
            stack.frames[stack.depth - 1].children += elapsed;
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    bool active = false;
};

} // namespace Native::OrderedTiming
