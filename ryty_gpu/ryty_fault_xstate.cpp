// SPDX-License-Identifier: GPL-2.0-or-later
// FIX-SGT-121 / FIX-SGT-194: the guest's AVX registers across a write fault.
//
// Ryty's exception handler (libc, built without AVX) offers each access fault to the renderer
// first, through this export. The renderer is built for x86-64-v3: its handler and the page
// tracking behind it run AVX code, which leaves the upper 128 bits of YMM0-15 zeroed or clobbered
// (vzeroupper, VEX-128 instructions). FIX-SGT-121: this function saves the AVX and AVX-512 state
// (XCR0 bits 2, 5, 6 and 7) with XSAVE before any AVX code runs and restores it after. It is
// built without AVX (RytyGpu.cmake) so that nothing before the XSAVE can touch those registers.
//
// FIX-SGT-194: Windows 11 24H2/25H2 kernels before 26100.7920 can build the exception's context
// without the AVX state (fault_xstate_check.h, JDK-8365196); the registers are then already zero
// here and stay zero after the resume. That cannot be undone here, so the first such context is
// reported on stderr with what to do (update Windows), and FIX-GT7-201 moves GT7's frame-job
// copies that crashed in RyGT issue 83 off the YMM registers.
//
// SHADGT_FAULT_XSTATE=0 calls the handler directly, as before FIX-SGT-121.
// DIAG-SGT-195: SHADGT_DIAG_FAULT_STORES=1 (diagnostics builds) logs the vector or page-crossing
// stores that fault (RytyGpuLogFaultStore in ryty_entry.cpp).
#include <windows.h>
#include <immintrin.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "common/diagnostics.h"
#include "ryty_gpu/fault_xstate_check.h"

extern "C" int RytyGpuHandleWriteFaultBody(void* exception_pointers);
extern "C" void RytyGpuLogFaultStore(void* exception_pointers, int result,
                                     const RytyFaultXState::CheckResult* check,
                                     unsigned long long in_use);

namespace {

// XCR0 components saved: AVX (YMM upper halves) and the three AVX-512 ones. 0 when the OS keeps
// none of them (no XSAVE, or no AVX), or when SHADGT_FAULT_XSTATE=0.
unsigned long long ComputeSavedComponents() {
    const char* env = std::getenv("SHADGT_FAULT_XSTATE");
    if (env && env[0] == '0') {
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
const unsigned long long g_saved_components = ComputeSavedComponents();
const bool g_log_stores = [] {
    const char* env = Common::DiagEnv("SHADGT_DIAG_FAULT_STORES");
    return env != nullptr && env[0] == '1';
}();

// The standard-form XSAVE area for those components ends below 2,700 bytes (AVX-512's last
// component ends at 2,688); 4 KB leaves room. On the faulting thread's own stack, so a fault
// inside the handler gets an area of its own.
constexpr std::size_t AreaSize = 4096;

// FIX-SGT-194: the first context without the AVX state says what it means; then the first 20 and
// every 1000th are counted on stderr (Ryty's log).
void NoteOmitted(const EXCEPTION_POINTERS* pointers, const RytyFaultXState::CheckResult& check,
                 unsigned long long in_use) {
    static std::atomic<unsigned long long> count{0};
    const unsigned long long n = ++count;
    if (n == 1) {
        std::fprintf(stderr,
                     "[fault] FIX-SGT-194: WINDOWS BUG: this Windows kernel leaves the AVX state out "
                     "of exception contexts (Windows 11 24H2/25H2 before update KB5077241, OS build "
                     "26100.7922 / 26200.7922). The game's vector registers are lost at such faults "
                     "and GT7 can crash mid-race. Install the latest Windows update.\n");
    }
    if (n > 20 && n % 1000 != 0) {
        return;
    }
    const auto* record = pointers->ExceptionRecord;
    std::fprintf(stderr,
                 "[fault] FIX-SGT-194: exception context without the AVX state (XSTATE_BV %#llx, "
                 "XCOMP_BV %#llx, left out %#llx, in use at the handler %#llx; %llu so far) at rip "
                 "%p, %s of %p\n",
                 static_cast<unsigned long long>(check.named),
                 static_cast<unsigned long long>(check.compacted),
                 static_cast<unsigned long long>(check.omitted), in_use, n,
                 record->ExceptionAddress,
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
    // From here on AVX code may run again until the XRSTOR.
    auto* pointers = static_cast<EXCEPTION_POINTERS*>(exception_pointers);
    if (pointers != nullptr && pointers->ExceptionRecord != nullptr &&
        pointers->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        unsigned long long in_use = 0;
        std::memcpy(&in_use, area + 512, sizeof(in_use));
        in_use &= mask;
        const auto check = RytyFaultXState::Check(pointers->ContextRecord, mask);
        if (check.outcome == RytyFaultXState::CheckResult::Omitted) {
            NoteOmitted(pointers, check, in_use);
        }
        if constexpr (Common::DiagnosticsBuilt) {
            if (g_log_stores) {
                RytyGpuLogFaultStore(exception_pointers, result, &check, in_use);
            }
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
