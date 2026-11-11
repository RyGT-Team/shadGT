// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// DIAG-056: NVIDIA Nsight Systems timeline ranges and marks. Built in only with the CMake option
// SHADGT_NVTX_INCLUDE in builds without NDEBUG (the nvtx3 headers of an Nsight Systems install);
// otherwise every call is empty. With the headers built in but no Nsight capture running, each
// call returns at once.
// DIAG-GTS-463: the renderer library (libSceVideoOut.prx) builds it in too, in Diagnostics builds
// (SHADGT_NVTX_INCLUDE, or -DRYGT_NVTX=ON to find the Nsight Systems headers): RyDiag's zones
// (ryty_diag.h) become ranges, thread names and a 64-bit value (a hash, an address) go along.
// The NVTX3 headers are included only by nvtx.cpp: their inline implementation uses Windows'
// MemoryBarrier macro, which vulkan.hpp undefines in the files that include both.

#include <string>
#include "common/types.h"

namespace Common::Nvtx {

#if defined(SHADGT_NVTX) && !defined(NDEBUG)
constexpr bool Enabled = true;

void Mark(const char* name);
/// A mark carrying a value (a shader hash, an address) as its payload.
void Mark(const char* name, u64 value);
/// A range that may end on another thread.
u64 Start(const char* name);
void End(u64 id);
void Push(const char* name);
/// A range carrying a value as its payload.
void Push(const char* name, u64 value);
void Pop();
/// The calling thread's name in the trace.
void NameThread(const char* name);

inline void Mark(const std::string& name) {
    Mark(name.c_str());
}
inline u64 Start(const std::string& name) {
    return Start(name.c_str());
}
#else
constexpr bool Enabled = false;

inline void Mark(const char*) {}
inline void Mark(const char*, u64) {}
inline void Mark(const std::string&) {}
inline u64 Start(const char*) {
    return 0;
}
inline u64 Start(const std::string&) {
    return 0;
}
inline void End(u64) {}
inline void Push(const char*) {}
inline void Push(const char*, u64) {}
inline void Pop() {}
inline void NameThread(const char*) {}
#endif

/// FIX-GTS-465: whether Nsight's injection (ToolsInjection64.dll) is in the process. Built in every
/// flavour, since a capture can run any build.
bool NsightInjected();

/// A range on the current thread for the scope.
class Scope {
public:
    explicit Scope(const char* name) {
        Push(name);
    }
    Scope(const char* name, u64 value) {
        Push(name, value);
    }
    ~Scope() {
        Pop();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace Common::Nvtx
