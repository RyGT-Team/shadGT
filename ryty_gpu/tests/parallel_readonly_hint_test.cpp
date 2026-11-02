#include "ryty_gpu/native/readonly_hint.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>
struct Region {uint64_t address,size,begin,capacity,handle;};
int main() {
    using Native::Parallel::ReadonlyHintOrScan;
    std::vector<std::pair<uint64_t,uint64_t>> ranges{{10,30},{40,80}};
    std::vector<Region> regions{{12,8,10,20,1},{50,8,40,30,2}};
    for (size_t rh : {size_t(0),size_t(1),size_t(255)}) for(size_t ih : {size_t(0),size_t(1),size_t(255)})
    for(uint64_t base : {12ull,50ull,70ull}) for(uint64_t size : {8ull,25ull})
    for(uint64_t handle : {1ull,2ull,3ull}) for(bool direct : {false,true}) {
        auto contained=[&](const auto& p){return p.first<=base && p.second>=base && size<=p.second-base;};
        auto match=[&](const auto& x){return x.address==base && x.size==size && base>=x.begin && x.handle==handle && base-x.begin<=x.capacity && size<=x.capacity-(base-x.begin);};
        bool hit=false;const bool old=std::any_of(ranges.begin(),ranges.end(),contained) && (std::any_of(regions.begin(),regions.end(),match)||direct);
        const bool now=ReadonlyHintOrScan(ranges,rh,contained,hit) && (ReadonlyHintOrScan(regions,ih,match,hit)||direct);
        assert(old==now);
    }
    std::reverse(ranges.begin(),ranges.end());std::reverse(regions.begin(),regions.end());
    bool hit=false;auto contains=[](const auto& p){return p.first<=12 && p.second>=20;};
    assert(ReadonlyHintOrScan(ranges,0,contains,hit) && !hit);
    regions[1].capacity=1;auto valid=[](const auto& x){return x.address==12 && x.size==8 && x.capacity>=10 && x.handle==1;};
    assert(!ReadonlyHintOrScan(regions,1,valid,hit));
}
