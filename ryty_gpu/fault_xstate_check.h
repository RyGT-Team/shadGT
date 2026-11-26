// SPDX-License-Identifier: GPL-2.0-or-later
// FIX-SGT-194: recognise an exception context that a Windows 11 kernel built without the AVX state.
//
// To dispatch a user-mode exception, Windows copies the thread's context, its XSAVE state
// included, below the faulting rsp. Build 26100 kernels before 26100.7920 (Windows 11 24H2 and
// 25H2 until KB5077241, OS build 26100/26200.7922; also while feature 58474193 is off, and
// Windows Server 2025) read the XSAVE header already at that spot first; when the stale bytes look
// like a compacted header (XCOMP_BV with bit 63 set, often a -0.0 or INT64_MIN the game left on
// its stack), the kernel keeps them and copies only the components they name (JDK-8365196). The
// thread's upper halves of ymm0-15 and its AVX-512 state are then already zero when any vectored
// handler runs, and the resume keeps them zero. In RyGT issue 83 that zeroed +0x28/+0x30 of a GT7
// frame job (FIX-GT7-201 works around those copies).
//
// This cannot be undone from a handler: the values are gone before it runs (the shim's XSAVE sees
// AVX not in use). The check only tells the player that Windows needs its update.
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace RytyFaultXState {

/// CONTEXT_EX, which follows the CONTEXT of a dispatched exception (not in the public headers).
struct ContextChunk {
    LONG offset;
    DWORD length;
};
struct ContextEx {
    ContextChunk all, legacy, xstate;
};

struct CheckResult {
    enum Outcome : std::uint8_t {
        Complete, ///< the context's layout holds every enabled vector component
        Omitted,  ///< a compacted header that leaves enabled vector components out (the bug)
        NoXState, ///< no XSTATE chunk in the context
    };
    Outcome outcome = Complete;
    std::uint64_t named = 0;     ///< XSTATE_BV of the context
    std::uint64_t compacted = 0; ///< XCOMP_BV of the context
    std::uint64_t omitted = 0;   ///< enabled vector components the layout leaves out
};

/// mask: the vector components the OS enables (XCR0 & 0xE4). Reads the context only.
inline CheckResult Check(const CONTEXT* context, std::uint64_t mask) {
    CheckResult result;
    if ((context->ContextFlags & CONTEXT_XSTATE) != CONTEXT_XSTATE) {
        result.outcome = CheckResult::NoXState;
        return result;
    }
    const auto* ex = reinterpret_cast<const ContextEx*>(context + 1);
    if (ex->xstate.length < 16) {
        result.outcome = CheckResult::NoXState;
        return result;
    }
    const auto* chunk = reinterpret_cast<const unsigned char*>(ex) + ex->xstate.offset;
    std::memcpy(&result.named, chunk, sizeof(result.named));
    std::memcpy(&result.compacted, chunk + 8, sizeof(result.compacted));
    constexpr std::uint64_t Compacted = 1ull << 63;
    if ((result.compacted & Compacted) != 0) {
        result.omitted = mask & ~result.compacted;
        if (result.omitted != 0) {
            result.outcome = CheckResult::Omitted;
        }
    }
    return result;
}

} // namespace RytyFaultXState
#endif
