// SPDX-License-Identifier: GPL-2.0-or-later
#include "ryty_gpu/native/adaptive_xxh3.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
using namespace Native::Parallel;
int main(){std::atomic<bool> go{};std::atomic<unsigned> errors{};std::array<std::thread,16> threads;std::vector<unsigned char> data(131200);for(size_t i=0;i<data.size();++i)data[i]=static_cast<unsigned char>((i*131u+i/17u)&255);
 for(auto& t:threads)t=std::thread([&]{while(!go.load(std::memory_order_acquire))std::this_thread::yield();for(int i=0;i<1000;++i){if(AdaptiveXXH3(data.data()+3,1025)!=XXH3_64bits(data.data()+3,1025)||AdaptiveXXH3Seed(data.data()+5,317,0x123456789abcdefULL)!=XXH3_64bits_withSeed(data.data()+5,317,0x123456789abcdefULL))++errors;}});go.store(true,std::memory_order_release);for(auto& t:threads)t.join();assert(!errors);
 size_t checked=0;for(size_t n=0;n<=1024;++n)for(size_t off=0;off<32;++off){auto p=data.data()+off;for(std::uint64_t seed:{0ULL,1ULL,0x123456789abcdefULL,~0ULL}){auto old=XXH3_64bits_withSeed(p,n,seed);assert(AdaptiveXXH3SeedWithCapability(p,n,seed,false)==old);assert(AdaptiveXXH3Seed(p,n,seed)==old);++checked;}assert(AdaptiveXXH3(p,n)==XXH3_64bits(p,n));assert(AdaptiveXXH3WithCapability(p,n,false)==XXH3_64bits(p,n));}
 for(size_t n:{2048u,4096u,65536u,131071u})for(size_t off:{0u,1u,7u,31u}){auto p=data.data()+off;assert(AdaptiveXXH3(p,n)==XXH3_64bits(p,n));for(std::uint64_t seed:{0ULL,1ULL,~0ULL})assert(AdaptiveXXH3Seed(p,n,seed)==XXH3_64bits_withSeed(p,n,seed));}
 SYSTEM_INFO si{};GetSystemInfo(&si);const size_t page=si.dwPageSize;auto mem=static_cast<unsigned char*>(VirtualAlloc(nullptr,page*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));assert(mem);DWORD oldprot;assert(VirtualProtect(mem+page,page,PAGE_NOACCESS,&oldprot));for(size_t i=0;i<page;++i)mem[i]=static_cast<unsigned char>(i*7);for(size_t n=0;n<=page;++n){auto p=mem+page-n;assert(AdaptiveXXH3(p,n)==XXH3_64bits(p,n));assert(AdaptiveXXH3Seed(p,n,17)==XXH3_64bits_withSeed(p,n,17));}VirtualFree(mem,0,MEM_RELEASE);std::printf("PASS checks=%zu host_avx512=%d concurrent_first_use=16 guard_page_lengths=%zu\n",checked,XXH512Available(),page+1);}
