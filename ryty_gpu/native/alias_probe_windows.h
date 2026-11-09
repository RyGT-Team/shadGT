#pragma once
#ifdef _WIN32
#include <windows.h>
#include <cstdint>
namespace Native::AliasProbe {
using UnmapFn = BOOL (WINAPI*)(HANDLE, void*, ULONG);
inline bool Cleanup(void* alias, std::uint32_t& error, UnmapFn unmap) {
    MEMORY_BASIC_INFORMATION info{}; SYSTEM_INFO sys{}; GetSystemInfo(&sys);
    if (VirtualQuery(alias,&info,sizeof(info)) != sizeof(info)) { error=GetLastError(); return false; }
    const auto ptr=reinterpret_cast<std::uintptr_t>(alias);
    if (!sys.dwAllocationGranularity || (sys.dwAllocationGranularity & (sys.dwAllocationGranularity-1)) ||
        info.State != MEM_COMMIT || info.Type != MEM_MAPPED || !info.AllocationBase ||
        reinterpret_cast<std::uintptr_t>(info.AllocationBase) !=
            (ptr & ~(std::uintptr_t(sys.dwAllocationGranularity)-1))) {
        error=ERROR_INVALID_DATA; return false;
    }
    if (!unmap(GetCurrentProcess(),info.AllocationBase,0)) { error=GetLastError(); return false; }
    return true;
}
}
#endif
