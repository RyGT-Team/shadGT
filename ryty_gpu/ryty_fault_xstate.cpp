// SPDX-License-Identifier: GPL-2.0-or-later
// FIX-SGT-121: the guest's AVX registers survive the renderer's write-fault handling.
//
// Ryty's exception handler (libc, built without AVX) offers each guest write fault to the
// renderer first, through this export. The renderer is built for x86-64-v3: its handler and the
// page tracking behind it run AVX code, which leaves the upper 128 bits of YMM0-15 zeroed or
// clobbered (vzeroupper, VEX-128 instructions). Windows puts them back on resume only from the
// AVX state in the exception's context. When the context carries none, the guest resumes with
// the renderer's values: in RyGT issue 83, GT7's frame-job producer loads two 256-bit values,
// a store of them faults on a tracked page, and the retried store writes zeros where the upper
// half of ymm1 belongs (the job's +0x28/+0x30), which a later job then reads as a null pointer.
// A test that drops CONTEXT_XSTATE from the context and clobbers YMM in an AVX handler loses
// those 16 bytes in 3 of 3 layouts; with this shim, in none.
//
// So this function saves the AVX and AVX-512 state (XCR0 bits 2, 5, 6 and 7) before any AVX
// code runs and restores it after. It is built without AVX (RytyGpu.cmake) so that nothing
// before the XSAVE can touch those registers. With the state in the context too, Windows then
// restores the same values. SHADGT_FAULT_XSTATE=0 calls the handler directly, as before.
#include <windows.h>
#include <immintrin.h>
#include <intrin.h>
#include <cstdint>
#include <cstdlib>

extern "C" int RytyGpuHandleWriteFaultBody(void* exception_pointers);

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

// The standard-form XSAVE area for those components ends below 2,700 bytes (AVX-512's last
// component ends at 2,688); 4 KB leaves room. On the faulting thread's own stack, so a fault
// inside the handler gets an area of its own.
constexpr std::size_t AreaSize = 4096;

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
