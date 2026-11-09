// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#ifdef _WIN32
#include <windows.h>
#endif
namespace Native {
// Kernel copy reports unreadable pages without recursively dispatching a guest fault while a
// page-watch shard is locked. On other hosts fault-ahead stays conservatively disabled.
inline bool CopyPageWithoutFault(void* destination, const void* source, std::size_t bytes) {
#ifdef _WIN32
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), source, destination, bytes, &copied) &&
           copied == bytes;
#else
    return false;
#endif
}
}
