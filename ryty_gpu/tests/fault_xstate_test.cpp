// SPDX-License-Identifier: GPL-2.0-or-later
// FIX-SGT-194 / FIX-GT7-201: on Windows 11 kernels with JDK-8365196, a write fault can be
// dispatched with a context whose XSAVE header names no AVX state, and the thread's YMM upper
// halves are already zero when the handler runs. This test checks that:
//  - RytyFaultXState::Check tells such a context from a complete one;
//  - GT7's frame-job copy (two 256-bit loads, stores at +0x08 and +0x18) loses the job's
//    +0x28..+0x37 when its store faults on such a kernel, and FIX-GT7-201's 128-bit copy does not.
//
// This machine's kernel may not have the bug, so the handler writes such a header into the
// context (CONTEXT_XSTATE set, XSTATE_BV 0, XCOMP_BV 0x8000000000000000) and zeroes the upper
// halves, which is what an affected kernel leaves the thread with.
//
// Build: clang-cl /O2 /EHsc /std:c++20 -mavx2 fault_xstate_test.cpp
#include <windows.h>
#include <immintrin.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "../fault_xstate_check.h"

namespace {

std::uint8_t* g_page2;
bool g_affected = false;
std::uint64_t g_mask = 0;
RytyFaultXState::CheckResult g_check;

LONG CALLBACK Handler(EXCEPTION_POINTERS* p) {
    if (p->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    VirtualProtect(g_page2, 4096, PAGE_READWRITE, &old);
    auto* ctx = p->ContextRecord;
    if (g_affected && (ctx->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE) {
        auto* ex = reinterpret_cast<RytyFaultXState::ContextEx*>(ctx + 1);
        auto* header = reinterpret_cast<std::uint64_t*>(reinterpret_cast<unsigned char*>(ex) + ex->xstate.offset);
        header[0] = 0;
        header[1] = 0x8000000000000000ull;
    }
    g_check = RytyFaultXState::Check(ctx, g_mask);
    if (g_affected) {
        _mm256_zeroupper(); // what an affected kernel leaves the thread with
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// GT7.exe+0x220f22b: vmovups ymm0,[rbp-0x60]; vmovups ymm1,[rbp-0x50];
// vmovups [r12+8],ymm0; vmovups [r12+0x18],ymm1.
__declspec(noinline) void CopyYmm(std::uint8_t* job, const std::uint8_t* src) {
    __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
    __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + 16));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(job + 0x08), a);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(job + 0x18), b);
}

// FIX-GT7-201's stub, instruction for instruction (src in place of rbp-0x60, job in r12).
__declspec(noinline) void CopyXmm(std::uint8_t* job, const std::uint8_t* src) {
    __asm__ volatile("vmovups (%1), %%xmm0\n\t"
                     "vmovups 0x10(%1), %%xmm1\n\t"
                     "vmovups %%xmm0, 0x08(%0)\n\t"
                     "vmovups %%xmm1, 0x18(%0)\n\t"
                     "vmovups 0x20(%1), %%xmm1\n\t"
                     "vmovups %%xmm1, 0x28(%0)\n\t"
                     "vmovups 0x10(%1), %%xmm1\n\t"
                     :
                     : "r"(job), "r"(src)
                     : "memory", "xmm0", "xmm1");
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
    struct Case { bool affected, xmm; const char* what; bool expect_kept; };
    const Case cases[] = {
        {false, false, "complete context, YMM copy (the game's)", true},
        {true, false, "AVX state left out, YMM copy (the game's)", false},
        {false, true, "complete context, XMM copy (FIX-GT7-201)", true},
        {true, true, "AVX state left out, XMM copy (FIX-GT7-201)", true},
    };
    int failures = 0;
    for (const auto& c : cases) {
        g_affected = c.affected;
        for (int job_off : {0x28, 0x10, 0x20, 0x08}) { // the job starts this far before the page edge
            std::memset(base, 0, 8192);
            DWORD old;
            VirtualProtect(g_page2, 4096, PAGE_READONLY, &old);
            g_check = {};
            std::uint8_t* job = g_page2 - job_off;
            (c.xmm ? CopyXmm : CopyYmm)(job, src);
            const bool kept = std::memcmp(job + 0x08, src, 48) == 0;
            const auto expected_check = c.affected ? RytyFaultXState::CheckResult::Omitted
                                                   : RytyFaultXState::CheckResult::Complete;
            const bool pass = kept == c.expect_kept && g_check.outcome == expected_check;
            std::printf("%-46s job %#04x before the edge: %s (expected %s), check %s (XCOMP_BV %#llx)\n", c.what,
                        job_off, kept ? "kept" : "LOST", c.expect_kept ? "kept" : "lost",
                        g_check.outcome == RytyFaultXState::CheckResult::Omitted ? "left out"
                        : g_check.outcome == RytyFaultXState::CheckResult::Complete ? "complete" : "no XSTATE",
                        (unsigned long long)g_check.compacted);
            failures += !pass;
        }
    }
    std::printf("%s\n", failures ? "RESULT: FAILED" : "RESULT: passed");
    return failures ? 1 : 0;
}
