// SPDX-License-Identifier: GPL-2.0-or-later
// FIX-SGT-121 / FIX-SGT-194: the guest's AVX registers survive a write fault.
//
// Ryty's exception handler (libc, built without AVX) offers each access fault to the renderer
// first, through this export. The renderer is built for x86-64-v3: its handler and the page
// tracking behind it run AVX code, which leaves the upper 128 bits of YMM0-15 zeroed or clobbered
// (vzeroupper, VEX-128 instructions). FIX-SGT-121: this function saves the AVX and AVX-512 state
// (XCR0 bits 2, 5, 6 and 7) with XSAVE before any AVX code runs and restores it after. It is
// built without AVX (RytyGpu.cmake) so that nothing before the XSAVE can touch those registers.
//
// That alone was not enough for RyGT issue 83 (GT7 crashing mid-race on Windows 11): Windows
// resumes the thread from the exception's context, and Windows 11 24H2/25H2 kernels before
// 26100.7920 can build that context with an XSAVE header that names no AVX state (a stale header
// on the stack, JDK-8365196). The resume then zeroes the upper halves whatever the registers
// hold. FIX-SGT-194: the components in use that the context left out are written into it from
// this function's XSAVE (fault_xstate_repair.h), for every access fault offered here, so the
// resume restores the guest's values. ryty_gpu/tests/fault_xstate_test.cpp shows the loss with
// FIX-SGT-121 alone and none with the repair.
//
// SHADGT_FAULT_XSTATE: unset or 1 = save, restore and repair; 2 = save and restore only
// (FIX-SGT-121 as it was); 0 = calls the handler directly, as before FIX-SGT-121.
// DIAG-SGT-195: SHADGT_DIAG_FAULT_STORES=1 (diagnostics builds) logs the vector or page-crossing
// stores that fault (RytyGpuLogFaultStore in ryty_entry.cpp).
#include <windows.h>
#include <immintrin.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "common/diagnostics.h"
#include "ryty_gpu/fault_xstate_repair.h"

extern "C" int RytyGpuHandleWriteFaultBody(void* exception_pointers);
extern "C" void RytyGpuLogFaultStore(void* exception_pointers, int result,
                                     const RytyFaultXState::RepairResult* repair);

namespace {

int ReadMode() {
    const char* env = std::getenv("SHADGT_FAULT_XSTATE");
    if (env && env[0] == '0') {
        return 0;
    }
    if (env && env[0] == '2') {
        return 2;
    }
    return 1;
}

// XCR0 components saved: AVX (YMM upper halves) and the three AVX-512 ones. 0 when the OS keeps
// none of them (no XSAVE, or no AVX), or when SHADGT_FAULT_XSTATE=0.
unsigned long long ComputeSavedComponents(int mode) {
    if (mode == 0) {
        return 0ull;
    }
    int info[4] = {};
    __cpuid(info, 1);
    const bool osxsave = (info[2] & (1 << 27)) != 0;
    if (!osxsave) {
        return 0ull;
    }
    return _xgetbv(0) & 0xE4ull;
}

// Set when the library loads, not in a fault: getenv and the C runtime's string functions may
// use AVX themselves, which would clobber the registers before they are saved.
const int g_mode = ReadMode();
const unsigned long long g_saved_components = ComputeSavedComponents(g_mode);
const bool g_repair = g_mode == 1;
const bool g_log_stores = [] {
    const char* env = Common::DiagEnv("SHADGT_DIAG_FAULT_STORES");
    return env != nullptr && env[0] == '1';
}();
// The CPUID table the repair reads, filled here rather than in the first fault.
const bool g_components_ready = (RytyFaultXState::Components(), true);

// The standard-form XSAVE area for those components ends below 2,700 bytes (AVX-512's last
// component ends at 2,688); 4 KB leaves room. On the faulting thread's own stack, so a fault
// inside the handler gets an area of its own.
constexpr std::size_t AreaSize = 4096;

// FIX-SGT-194: every repaired context is a resume that would have lost AVX state. The first 20
// and then every 1000th are reported on stderr (Ryty's log), with what the kernel's header said.
void NoteRepair(const EXCEPTION_POINTERS* pointers, const RytyFaultXState::RepairResult& repair) {
    static std::atomic<unsigned long long> count{0};
    const unsigned long long n = ++count;
    if (n > 20 && n % 1000 != 0) {
        return;
    }
    const auto* record = pointers->ExceptionRecord;
    std::fprintf(stderr,
                 "[fault] FIX-SGT-194: the exception context left out AVX state in use (in use %#llx, "
                 "XSTATE_BV %#llx, XCOMP_BV %#llx); put back before the resume (%llu so far) at rip "
                 "%p, %s of %p\n",
                 static_cast<unsigned long long>(repair.live),
                 static_cast<unsigned long long>(repair.named),
                 static_cast<unsigned long long>(repair.compacted), n, record->ExceptionAddress,
                 record->NumberParameters >= 2 && record->ExceptionInformation[0] == 1 ? "write"
                                                                                       : "access",
                 reinterpret_cast<void*>(record->NumberParameters >= 2
                                             ? record->ExceptionInformation[1]
                                             : 0));
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("xsave")))
#endif
int CallKeepingAvxState(void* exception_pointers, unsigned long long mask) {
    alignas(64) unsigned char area[AreaSize];
    // The XSAVE header (bytes 512-575): XCOMP_BV must be 0 for XRSTOR's standard form, and
    // XSAVE leaves it as it finds it.
    for (std::size_t i = 512; i < 576; ++i) {
        area[i] = 0;
    }
    _xsave64(area, mask);
    const int result = RytyGpuHandleWriteFaultBody(exception_pointers);
    // From here on AVX code may run again until the XRSTOR: the area holds the guest's state.
    auto* pointers = static_cast<EXCEPTION_POINTERS*>(exception_pointers);
    RytyFaultXState::RepairResult repair;
    if (g_repair && pointers != nullptr && pointers->ExceptionRecord != nullptr &&
        pointers->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        repair = RytyFaultXState::Repair(pointers->ContextRecord, area, mask);
        if (repair.outcome == RytyFaultXState::RepairResult::Repaired) {
            NoteRepair(pointers, repair);
        }
    }
    if constexpr (Common::DiagnosticsBuilt) {
        if (g_log_stores) {
            RytyGpuLogFaultStore(exception_pointers, result, &repair);
        }
    }
    _xrstor64(area, mask);
    return result;
}

} // namespace

extern "C" __declspec(dllexport) int RytyGpuHandleWriteFault(void* exception_pointers) {
    const unsigned long long mask = g_saved_components;
    if (mask == 0) {
        return RytyGpuHandleWriteFaultBody(exception_pointers);
    }
    return CallKeepingAvxState(exception_pointers, mask);
}
