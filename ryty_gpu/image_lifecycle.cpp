// DIAG-378-GTS: bounded opt-in CPU lifecycle capture for Windows RyGTS.
#include "ryty_gpu/image_lifecycle.h"
#if defined(_WIN32) && defined(RYTY_GPU)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <memory>

namespace ImageLife {
namespace {
constexpr std::uint64_t Capacity=131072;
struct Event {
    std::atomic<bool> committed{false};
    std::uint64_t qpc{}, id{}, a{}, b{}, c{}, d{}, e{};
    DWORD tid{};
    const char* kind{};
    char phase{};
};
struct State {
    std::atomic<bool> open{false};
    std::atomic<std::uint64_t> active{0}, next{0}, dropped{0}, ids{0};
    std::unique_ptr<Event[]> events{new Event[Capacity]};
    wchar_t marker[MAX_PATH]{}, output[MAX_PATH]{};
    LARGE_INTEGER frequency{}, start{}, end{};
};
State* state=nullptr; // initialized once before renderer startup; intentionally process lifetime

bool Append(char phase, std::uint64_t id, const char* kind, std::uint64_t a=0,
            std::uint64_t b=0, std::uint64_t c=0, std::uint64_t d=0, std::uint64_t e=0) {
    State* s=state;
    if (!s || !s->open.load()) return false;
    s->active.fetch_add(1);
    if (!s->open.load()) { s->active.fetch_sub(1); return false; }
    const auto index=s->next.fetch_add(1);
    if(index>=Capacity) { s->dropped.fetch_add(1); s->active.fetch_sub(1); return false; }
    auto& item=s->events[index];
    LARGE_INTEGER q{}; QueryPerformanceCounter(&q);
    item.qpc=q.QuadPart; item.id=id; item.a=a; item.b=b; item.c=c; item.d=d; item.e=e;
    item.tid=GetCurrentThreadId(); item.kind=kind; item.phase=phase;
    item.committed.store(true, std::memory_order_release);
    s->active.fetch_sub(1);
    return true;
}
DWORD WINAPI Worker(void* raw) {
    auto* s=static_cast<State*>(raw);
    while (GetFileAttributesW(s->marker)==INVALID_FILE_ATTRIBUTES) Sleep(100);
    DeleteFileW(s->marker);
    QueryPerformanceCounter(&s->start);
    s->open.store(true);
    Sleep(25000);
    s->open.store(false);
    QueryPerformanceCounter(&s->end);
    while(s->active.load()!=0) Sleep(1);
    if (auto* f=_wfopen(s->output,L"w")) {
        std::fprintf(f,"# pid=%lu,qpc_frequency=%lld,start=%lld,end=%lld,capacity=%llu,attempted=%llu,dropped=%llu,slow_threshold_us=200\n",
                     GetCurrentProcessId(),s->frequency.QuadPart,s->start.QuadPart,s->end.QuadPart,
                     Capacity,s->next.load(),s->dropped.load());
        std::fprintf(f,"qpc,tid,phase,id,kind,a,b,c,d,e\n");
        const auto count=(std::min)(s->next.load(),Capacity);
        for(std::uint64_t i=0;i<count;++i) {
            const auto& v=s->events[i];
            if(v.committed.load(std::memory_order_acquire))
                std::fprintf(f,"%llu,%lu,%c,%llu,%s,%llu,%llu,%llu,%llu,%llu\n",
                             v.qpc,v.tid,v.phase,v.id,v.kind,v.a,v.b,v.c,v.d,v.e);
        }
        std::fclose(f);
    }
    return 0;
}
}
void Initialize() {
    char enabled[3]{};
    if(GetEnvironmentVariableA("RYGT_IMAGE_LIFECYCLE_TRACE",enabled,sizeof(enabled))!=1 || enabled[0]!='1') return;
    auto* s=new State;
    wchar_t exe[MAX_PATH]{};
    const DWORD length=GetModuleFileNameW(nullptr,exe,MAX_PATH);
    if(!length || length>=MAX_PATH) { delete s; return; }
    wchar_t* slash=wcsrchr(exe,L'\\');
    if(!slash) { delete s; return; }
    *slash=0;
    wchar_t logs[MAX_PATH]{};
    if(std::swprintf(logs,MAX_PATH,L"%ls\\logs",exe)<0) { delete s; return; }
    CreateDirectoryW(logs,nullptr);
    if(std::swprintf(s->marker,MAX_PATH,L"%ls\\image-lifecycle-%lu.start",logs,GetCurrentProcessId())<0 ||
       std::swprintf(s->output,MAX_PATH,L"%ls\\image-lifecycle-%lu.csv",logs,GetCurrentProcessId())<0) { delete s; return; }
    QueryPerformanceFrequency(&s->frequency);
    HMODULE pinned{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&Worker),&pinned)) { delete s; return; }
    state=s;
    HANDLE thread=CreateThread(nullptr,0,Worker,s,0,nullptr);
    if(thread) CloseHandle(thread);
}
std::uint64_t Begin(const char* kind,std::uint64_t a,std::uint64_t b,std::uint64_t c,std::uint64_t d,std::uint64_t e) {
    State* s=state;
    if(!s || !s->open.load()) return 0;
    const auto id=s->ids.fetch_add(1)+1;
    return Append('B',id,kind,a,b,c,d,e) ? id : 0;
}
void End(std::uint64_t id,const char* kind,std::uint64_t a,std::uint64_t b) {
    if(id) Append('E',id,kind,a,b);
}
std::uint64_t SlowStart() {
    State* s=state;
    if(!s || !s->open.load()) return 0;
    LARGE_INTEGER q{}; QueryPerformanceCounter(&q);
    return q.QuadPart;
}
void SlowEnd(std::uint64_t start,const char* kind,std::uint64_t address,std::uint64_t size) {
    State* s=state;
    if(!start || !s || !s->open.load()) return;
    LARGE_INTEGER q{}; QueryPerformanceCounter(&q);
    if(q.QuadPart-start >= s->frequency.QuadPart/5000)
        Append('S',0,kind,start,address,size);
}
}
#endif

