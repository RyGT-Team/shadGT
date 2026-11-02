#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
using u8=std::uint8_t; using u64=std::uint64_t;
struct View { const u8* ptr; const u8* data() const { return ptr; } };
struct Run { u64 at; std::vector<u8> bytes; bool operator==(const Run&) const = default; };
struct Result { std::vector<Run> runs; u64 changed=0; bool threw=false; };
static Result scan(const u8* nowptr,const u8* oldptr,u64 page_bytes,int throw_at=-1) {
    View now{nowptr}, old{oldptr}; auto* before=&old; constexpr u64 page=0;
    Result result;
    auto copy_snapshot=[&](u64 at,const u8* data,u64 n) {
        if(static_cast<int>(result.runs.size())==throw_at) throw std::runtime_error("copy");
        result.runs.push_back({at,{data,data+n}});
    };
    try {
#include "region-changed-runs-under-test.inc"
        result.changed=changed;
    } catch(const std::runtime_error&) { result.threw=true; }
    return result;
}
static Result reference(const u8* now,const u8* before,u64 size,int throw_at=-1) {
    Result result; u64 start=0,end=0; bool in=false;
    auto emit=[&] { if(static_cast<int>(result.runs.size())==throw_at) throw std::runtime_error("copy"); result.runs.push_back({start,{now+start,now+end}}); result.changed+=end-start; };
    try {
        for(u64 at=0;at<size;at+=16) {
            const u64 n=std::min<u64>(16,size-at);
            if(std::memcmp(now+at,before+at,n)==0) continue;
            if(in && at-end<64) { end=at+n; continue; }
            if(in) emit();
            in=true; start=at; end=at+n;
        }
        if(in) emit();
    } catch(const std::runtime_error&) { result.threw=true; result.changed=0; }
    return result;
}
static void check(const u8* a,const u8* b,u64 n) {
    auto x=scan(a,b,n), y=reference(a,b,n); assert(x.runs==y.runs); assert(x.changed==y.changed); assert(!x.threw);
    if(x.runs.size()>1) { auto q=scan(a,b,n,1), r=reference(a,b,n,1); assert(q.threw && r.threw && q.runs==r.runs); }
}
struct Guarded {
    u8* base; size_t page;
    Guarded() {
#ifdef _WIN32
        SYSTEM_INFO si; GetSystemInfo(&si); page=si.dwPageSize;
        base=static_cast<u8*>(VirtualAlloc(nullptr,page*3,MEM_RESERVE,PAGE_NOACCESS)); assert(base);
        assert(VirtualAlloc(base+page,page,MEM_COMMIT,PAGE_READWRITE));
#else
        page=static_cast<size_t>(sysconf(_SC_PAGESIZE)); base=static_cast<u8*>(mmap(nullptr,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)); assert(base!=MAP_FAILED); assert(mprotect(base+page,page,PROT_READ|PROT_WRITE)==0);
#endif
        assert(page>=4096);
    }
    ~Guarded() {
#ifdef _WIN32
        VirtualFree(base,0,MEM_RELEASE);
#else
        munmap(base,page*3);
#endif
    }
};
int main() {
    Guarded ga,gb; std::mt19937_64 rng(0x173081);
    for(size_t n=0;n<=4096;++n) for(int side=0;side<2;++side) {
        u8* a=ga.base+ga.page+(side ? ga.page-n : 0); u8* b=gb.base+gb.page+(side ? gb.page-n : 0);
        std::fill_n(a,n,u8{0}); std::fill_n(b,n,u8{0}); check(a,b,n);
        if(n) { a[0]=1; a[n-1]=1; check(a,b,n); for(size_t j=0;j<n/16+1;++j) a[rng()%n]=static_cast<u8>(1+rng()%255); check(a,b,n); }
    }
    auto* a=ga.base+ga.page; auto* b=gb.base+gb.page;
    std::fill_n(b,4096,u8{0});
    for(size_t i=0;i<4096;++i) { std::fill_n(a,4096,u8{0}); a[i]=1; check(a,b,4096); }
    for(size_t gap : {48u,64u,80u}) { std::fill_n(a,4096,u8{0}); a[0]=1; a[16+gap]=2; auto result=scan(a,b,4096); check(a,b,4096); assert(result.runs.size()==(gap<64?1u:2u)); }
    for(int i=0;i<4000;++i) { size_t n=rng()%4097; std::fill_n(a,n,u8{0}); for(size_t j=0;j<n/8;++j) if(n) a[rng()%n]=static_cast<u8>(rng()); check(a,b,n); }
}
