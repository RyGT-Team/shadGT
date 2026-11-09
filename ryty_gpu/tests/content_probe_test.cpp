#include "../native/content_probe.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>
static bool fail_alloc=false;
void* operator new(std::size_t n) { if(fail_alloc) throw std::bad_alloc(); if(auto p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
int main() {
    using namespace Native::ContentProbe;
    constexpr std::size_t n=4ull<<20;
    std::vector<std::uint8_t> source(n,7), dest(n,0); Scratch scratch;
    unsigned copies=0, hashes=0, forwards=0;
    auto copy=[&](void* d,const void* s,std::size_t bytes){++copies; std::memcpy(d,s,bytes); return true;};
    auto hash=[&](const void* s,std::size_t bytes){++hashes; return XXH3_64bits(s,bytes);};
    auto forward=[&](void* d,const void* s,std::size_t bytes){++forwards; std::memcpy(d,s,bytes);};
    assert(Try(scratch,false,n,n,source.data(),dest.data(),copy,hash,forward).status==Status::Unavailable);
    assert(Try(scratch,true,n-1,n-1,source.data(),dest.data(),copy,hash,forward).status==Status::Unavailable);
    assert(Try(scratch,true,(64ull<<20)+1,(64ull<<20)+1,source.data(),dest.data(),copy,hash,forward).status==Status::Unavailable);
    assert(Try(scratch,true,n,n-1,source.data(),dest.data(),copy,hash,forward).status==Status::Unavailable);
    fail_alloc=true; auto fail=Try(scratch,true,n,n,source.data(),dest.data(),copy,hash,forward); fail_alloc=false;
    assert(fail.status==Status::Unavailable && copies==0 && hashes==0 && forwards==0 && !scratch.busy);
    auto bad=[&](void*,const void*,std::size_t){++copies;return false;};
    assert(Try(scratch,true,n,n,source.data(),dest.data(),bad,hash,forward).status==Status::CopyFailed);
    assert(hashes==0 && forwards==0 && dest[0]==0);
    auto result=Try(scratch,true,n,n,source.data(),dest.data(),copy,hash,forward);
    assert(result.status==Status::Valid && result.hash==XXH3_64bits(dest.data(),dest.size()) && dest==source);
    const auto prior=result.hash;
    source[0]=9; result=Try(scratch,true,n,n,source.data(),dest.data(),copy,hash,forward);
    assert(result.status==Status::Valid && result.hash==XXH3_64bits(dest.data(),dest.size()) && result.hash!=prior && dest==source);
    scratch.busy=true; assert(Try(scratch,true,n,n,source.data(),dest.data(),copy,hash,forward).status==Status::Unavailable);
}
