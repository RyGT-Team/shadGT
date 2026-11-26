// SPDX-License-Identifier: GPL-2.0-or-later
// FIX-SGT-194: put back the AVX state an exception context left out (Windows 11 24H2/25H2 kernels).
//
// To dispatch a user-mode exception, Windows copies the thread's context, its XSAVE state
// included, below the faulting rsp. Build 26100 kernels before 26100.7920 (Windows 11 24H2 and
// 25H2 until KB5077241, and while feature 58474193 is off) read the XSAVE header already at that
// spot first; when the stale bytes look like a compacted header, the kernel keeps them and copies
// only the components they name (JDK-8365196). The context then names no AVX state although the
// thread uses it, and resuming from it puts the upper halves of ymm0-15 (and the AVX-512 state)
// in their initial state, zero. Restoring the live registers before returning (FIX-SGT-121) does
// not help: the resume itself zeroes them.
//
// In RyGT issue 83, GT7's frame-job producer loads two 256-bit values and stores them into a
// job; when the job straddles a page the renderer tracks, a YMM store is the first write to that
// page and faults, and the job's +0x28/+0x30 come back zero.
//
// The registers are still right when the handler runs (the kernel returns to user mode with the
// thread's own saved state), so the fault shim, which saves them with XSAVE before any AVX code,
// writes the components the context left out back into its XSAVE area and corrects the header.
// No Windows call: this runs inside the exception handler, built without AVX.
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstring>

namespace RytyFaultXState {

/// What a repair found: the components in use that the context named (or not) and what was done.
struct RepairResult {
    enum Outcome : std::uint8_t {
        Complete,     ///< the context names every component in use
        Repaired,     ///< it did not; the missing ones were written into it
        NoXState,     ///< the context carries no XSTATE chunk (live registers survive the resume)
        TooSmall,     ///< the chunk has no room for the full layout: left as it was
        Foreign,      ///< the stale header names components this repair does not save: left as it was
    };
    Outcome outcome = Complete;
    std::uint64_t live = 0;     ///< components in use (the shim's XSAVE header)
    std::uint64_t named = 0;    ///< XSTATE_BV of the context as found
    std::uint64_t compacted = 0; ///< XCOMP_BV of the context as found
};

/// CONTEXT_EX, which follows the CONTEXT of a dispatched exception (not in the public headers).
struct ContextChunk {
    LONG offset;
    DWORD length;
};
struct ContextEx {
    ContextChunk all, legacy, xstate;
};

/// Standard-form offset, size and whether the compacted form aligns it to 64 bytes (CPUID 0xD).
struct Component {
    std::uint32_t standard_offset = 0, size = 0;
    bool align64 = false;
};

inline const Component* Components() {
    static Component table[64] = {};
    static bool filled = false;
    if (!filled) {
        for (int i = 2; i < 64; ++i) {
            int info[4] = {};
            __cpuidex(info, 0xD, i);
            table[i].size = static_cast<std::uint32_t>(info[0]);
            table[i].standard_offset = static_cast<std::uint32_t>(info[1]);
            table[i].align64 = (info[2] & 2) != 0;
        }
        filled = true;
    }
    return table;
}

/// context: the dispatched exception's context. area: a standard-form XSAVE area of `mask`
/// (components 2 and up) taken while the registers still held the guest's values.
inline RepairResult Repair(CONTEXT* context, const unsigned char* area, std::uint64_t mask) {
    RepairResult result;
    std::uint64_t saved_bv;
    std::memcpy(&saved_bv, area + 512, sizeof(saved_bv));
    result.live = saved_bv & mask;
    if ((context->ContextFlags & CONTEXT_XSTATE) != CONTEXT_XSTATE) {
        result.outcome = RepairResult::NoXState;
        return result;
    }
    auto* ex = reinterpret_cast<ContextEx*>(context + 1);
    auto* chunk = reinterpret_cast<unsigned char*>(ex) + ex->xstate.offset;
    const std::uint64_t length = ex->xstate.length;
    if (length < 64) {
        result.outcome = RepairResult::TooSmall;
        return result;
    }
    std::uint64_t header[2];
    std::memcpy(header, chunk, sizeof(header));
    result.named = header[0];
    result.compacted = header[1];
    const std::uint64_t missing = result.live & ~header[0];
    if (missing == 0) {
        return result;
    }
    // The chunk starts at the XSAVE header: offset 512 of the area the offsets count from.
    const Component* components = Components();
    constexpr std::uint64_t Compacted = 1ull << 63;
    std::uint64_t place[64] = {};
    std::uint64_t layout = 0;
    if ((header[1] & Compacted) != 0) {
        // Only components this repair rewrites may move; anything else keeps the header as is.
        if ((header[1] & ~Compacted & ~3ull & ~mask) != 0) {
            result.outcome = RepairResult::Foreign;
            return result;
        }
        layout = (header[1] & ~Compacted) | mask;
        std::uint64_t offset = 576;
        for (int i = 2; i < 64; ++i) {
            if ((layout >> i) & 1) {
                if (components[i].align64) {
                    offset = (offset + 63) & ~63ull;
                }
                place[i] = offset;
                offset += components[i].size;
            }
        }
        if (offset - 512 > length) {
            result.outcome = RepairResult::TooSmall;
            return result;
        }
    } else {
        for (int i = 2; i < 64; ++i) {
            if ((mask >> i) & 1) {
                place[i] = components[i].standard_offset;
                if (place[i] + components[i].size - 512 > length) {
                    result.outcome = RepairResult::TooSmall;
                    return result;
                }
            }
        }
    }
    for (int i = 2; i < 64; ++i) {
        if ((result.live >> i) & 1) {
            std::memcpy(chunk + (place[i] - 512), area + components[i].standard_offset,
                        components[i].size);
        }
    }
    header[0] = (header[0] & ~mask) | result.live;
    if ((header[1] & Compacted) != 0) {
        header[1] = Compacted | (header[1] & 3ull) | layout;
    }
    std::memcpy(chunk, header, sizeof(header));
    result.outcome = RepairResult::Repaired;
    return result;
}

} // namespace RytyFaultXState
#endif
