#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "common/thread.h"
int main() {
    unsigned count = 0;
    bool saw_self = false;
    const auto self = GetCurrentThreadId();
    const bool walked = Common::ForEachProcessThread(THREAD_QUERY_LIMITED_INFORMATION,
        [&](void* handle, unsigned long id) {
            if (!handle || GetThreadId(static_cast<HANDLE>(handle)) != id) std::abort();
            ++count;
            saw_self |= id == self;
            return false;
        });
    std::printf("walked=%d count=%u saw_self=%d\n", walked, count, saw_self);
    if (!walked || !count || !saw_self) return 1;
    ULONG bytes=0;
    GetSystemCpuSetInformation(nullptr,0,&bytes,GetCurrentProcess(),0);
    if (!bytes) { std::puts("CPU sets unavailable; restriction check skipped"); return 0; }
    std::vector<unsigned char> data(bytes);
    if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(data.data()),bytes,&bytes,GetCurrentProcess(),0)) return 2;
    const auto* first=reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(data.data());
    const ULONG selected=first->CpuSet.Id;
    if (std::getenv("RYGT_POLICY_TEST_LATE")) {
        _putenv_s("RYGT_COMPILE_CORES","0");
        _putenv_s("RYGT_CORE_OWNERS","2");
        Common::UpdateCoreOwner(1,true);
        ULONG needed=0;
        GetProcessDefaultCpuSets(GetCurrentProcess(),nullptr,0,&needed);
        if (!needed) { std::puts("owner policy unavailable; late-change check skipped"); return 0; }
        std::vector<ULONG> owned(needed);
        if (!GetProcessDefaultCpuSets(GetCurrentProcess(),owned.data(),needed,&needed)) return 10;
        ULONG changed=selected;
        if (owned.size()==1 && owned[0]==changed) { std::puts("no distinct selection; late-change check skipped"); return 0; }
        if (!SetProcessDefaultCpuSets(GetCurrentProcess(),&changed,1)) return 11;
        Common::UpdateCoreOwner(1,false);
        ULONG actual=0;needed=0;
        if (!GetProcessDefaultCpuSets(GetCurrentProcess(),&actual,1,&needed) || needed!=1 || actual!=changed) return 12;
        needed=0;
        if (!GetThreadSelectedCpuSets(GetCurrentThread(),nullptr,0,&needed) || needed!=0) return 13;
        std::puts("late external process selection preserved and owned thread selection released");
        return 0;
    }
    if (!SetProcessDefaultCpuSets(GetCurrentProcess(),&selected,1)) return 3;
    if (!SetThreadSelectedCpuSets(GetCurrentThread(),&selected,1)) return 5;
    Common::PreferCompileCores();
    Common::UpdateCoreOwner(1,true);
    Common::ReleaseCompileCores();
    Common::UpdateCoreOwner(1,false);
    ULONG actual=0,needed=0;
    if (!GetProcessDefaultCpuSets(GetCurrentProcess(),&actual,1,&needed) || needed!=1 || actual!=selected) return 4;
    actual=needed=0;
    if (!GetThreadSelectedCpuSets(GetCurrentThread(),&actual,1,&needed) || needed!=1 || actual!=selected) return 6;
    const int before=GetThreadPriority(GetCurrentThread());
    if (before==THREAD_PRIORITY_ERROR_RETURN) return 7;
    Common::BeginForegroundCompile();
    Common::BeginForegroundCompile();
    Common::EndForegroundCompile();
    Common::EndForegroundCompile();
    if (GetThreadPriority(GetCurrentThread())!=before) return 8;
    actual=needed=0;
    if (!GetThreadSelectedCpuSets(GetCurrentThread(),&actual,1,&needed) || needed!=1 || actual!=selected) return 9;
    std::puts("existing process and thread CPU selections preserved");
    return 0;
}
