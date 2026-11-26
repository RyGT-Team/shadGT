// SPDX-License-Identifier: GPL-2.0-or-later
// FIX-SGT-194: a write fault resumed from a context whose XSAVE header names no AVX state loses
// the upper halves of the YMM registers, with or without FIX-SGT-121's save and restore, and
// RytyFaultXState::Repair keeps them.
//
// GT7's frame-job producer (GT7.exe+0x220f22b, also +0x239b2eb): two 256-bit loads from the
// stack, then vmovups [r12+8], ymm0 and vmovups [r12+0x18], ymm1. The job here straddles a
// write-protected page, as in RyGT issue 83. Affected Windows 11 kernels build that context from
// a stale header on the stack (JDK-8365196); this test writes such a header into the context
// itself (CONTEXT_XSTATE stays set, XSTATE_BV 0, XCOMP_BV 0x8000000000000000), which is what
// those kernels hand the handler, so it runs on any Windows. Those kernels also reset the omitted
// components on resume, where older ones keep the live registers; the handler zeroes the upper
// halves last to model that.
//
// Build: clang-cl /O2 /EHsc /std:c++20 -mavx2 -I.. fault_xstate_test.cpp
// (the shim function is built without AVX by its target attribute, like ryty_fault_xstate.cpp.)
#include <windows.h>
#include <immintrin.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "../fault_xstate_repair.h"

namespace {

std::uint8_t* g_page2;
int g_simulate_stale = 0; // 1: the context names no AVX (affected kernel)
int g_mode = 0;           // 0: no shim; 1: FIX-SGT-121 save/restore; 2: + FIX-SGT-194 repair
std::uint64_t g_mask = 0;
bool g_live_upper_ok = false;
RytyFaultXState::RepairResult g_result;
bool g_locate_matches = true;

// The renderer's handler: AVX code that leaves the upper halves clobbered.
__declspec(noinline) void AvxBody() {
    volatile __m256 junk = _mm256_set1_ps(-7.0f);
    (void)junk;
    _mm256_zeroupper();
}

__attribute__((target("xsave"), noinline)) void Shim(EXCEPTION_POINTERS* p) {
    alignas(64) unsigned char area[4096];
    for (int i = 512; i < 576; ++i) area[i] = 0;
    _xsave64(area, g_mask);
    // The second load's upper half (src[32..48) = c0..cf) in some register's upper half, as the
    // shim saw it (AVX area: standard offset 576, 16 bytes per register).
    g_live_upper_ok = false;
    for (int r = 0; r < 16; ++r) {
        bool same = true;
        for (int i = 0; i < 16; ++i) same &= area[576 + 16 * r + i] == 0xC0 + i;
        g_live_upper_ok |= same;
    }
    AvxBody();
    if (g_mode == 2) {
        g_result = RytyFaultXState::Repair(p->ContextRecord, area, g_mask);
    }
    _xrstor64(area, g_mask);
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* p) {
    if (p->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    VirtualProtect(g_page2, 4096, PAGE_READWRITE, &old);
    auto* ctx = p->ContextRecord;
    if (g_simulate_stale && (ctx->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE) {
        auto* ex = reinterpret_cast<RytyFaultXState::ContextEx*>(ctx + 1);
        auto* header = reinterpret_cast<std::uint64_t*>(reinterpret_cast<unsigned char*>(ex) + ex->xstate.offset);
        header[0] = 0;
        header[1] = 0x8000000000000000ull;
    }
    if (g_mode == 0) {
        AvxBody();
    } else {
        Shim(p);
    }
    // An affected kernel resets the components the context omits when it resumes (JDK-8365196);
    // older kernels leave them as the registers hold them. Zeroing the upper halves here makes
    // the outcome depend only on the context, as on an affected kernel.
    _mm256_zeroupper();
    if (g_mode == 2 && g_result.outcome == RytyFaultXState::RepairResult::Repaired) {
        // The repaired layout must be the one Windows reads.
        auto* ex = reinterpret_cast<RytyFaultXState::ContextEx*>(ctx + 1);
        auto* chunk = reinterpret_cast<unsigned char*>(ex) + ex->xstate.offset;
        DWORD len = 0;
        auto* avx = static_cast<unsigned char*>(LocateXStateFeature(ctx, XSTATE_AVX, &len));
        std::uint64_t comp;
        std::memcpy(&comp, chunk + 8, 8);
        const auto* c = RytyFaultXState::Components();
        const std::uint64_t expected = (comp >> 63) ? 576 : c[2].standard_offset;
        g_locate_matches = avx == chunk + (expected - 512) && len == c[2].size;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

__declspec(noinline) void Produce(std::uint8_t* job, const std::uint8_t* src) {
    __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
    __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + 16));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(job + 0x08), a);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(job + 0x18), b);
}

} // namespace

int main() {
    int info[4];
    __cpuid(info, 1);
    if (!(info[2] & (1 << 27)) || !(info[2] & (1 << 28))) {
        std::printf("no AVX\n");
        return 0;
    }
    g_mask = _xgetbv(0) & 0xE4ull;
    AddVectoredExceptionHandler(1, Handler);
    auto* base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    g_page2 = base + 4096;
    alignas(32) std::uint8_t src[48];
    for (int i = 0; i < 48; ++i) src[i] = std::uint8_t(0xA0 + i);
    struct Case { int stale, mode; const char* what; bool expect_ok; };
    const Case cases[] = {
        {0, 0, "complete context, no shim", true},
        {1, 0, "context names no AVX, no shim", false},
        {1, 1, "context names no AVX, FIX-SGT-121 save/restore", false},
        {1, 2, "context names no AVX, FIX-SGT-194 repair", true},
        {0, 2, "complete context, FIX-SGT-194 repair (nothing to do)", true},
    };
    int failures = 0;
    for (const auto& c : cases) {
        g_simulate_stale = c.stale;
        g_mode = c.mode;
        for (int job_off : {0x28, 0x10, 0x20, 0x08}) { // job starts this far before the page edge
            std::memset(base, 0, 8192);
            DWORD old;
            VirtualProtect(g_page2, 4096, PAGE_READONLY, &old);
            g_result = {};
            g_locate_matches = true;
            std::uint8_t* job = g_page2 - job_off;
            Produce(job, src);
            const bool ok = std::memcmp(job + 0x08, src, 16) == 0 && std::memcmp(job + 0x18, src + 16, 32) == 0;
            const bool pass = ok == c.expect_ok && g_locate_matches;
            std::printf("%-55s job %#04x before the edge: %s (expected %s)%s%s\n", c.what, job_off,
                        ok ? "kept" : "LOST", c.expect_ok ? "kept" : "lost",
                        c.mode ? (g_live_upper_ok ? ", live upper right at the shim" : ", live upper WRONG at the shim") : "",
                        g_locate_matches ? "" : ", LocateXStateFeature disagrees");
            if (c.mode == 2) {
                std::printf("    repair outcome %d, live %#llx, named %#llx, XCOMP_BV %#llx\n", int(g_result.outcome),
                            (unsigned long long)g_result.live, (unsigned long long)g_result.named,
                            (unsigned long long)g_result.compacted);
            }
            failures += !pass;
        }
    }
    std::printf("%s\n", failures ? "RESULT: FAILED" : "RESULT: passed");
    return failures ? 1 : 0;
}
