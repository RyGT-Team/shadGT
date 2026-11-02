// SPDX-License-Identifier: GPL-2.0-or-later
#define RYGT_COHERENCE_TEST
#include "ryty_gpu/native/direct_coherence_registry.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <random>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using U=std::uint64_t;
using Range=std::pair<U,U>; // half-open [begin,end)
template<class Registry> bool Reference(Registry& registry,const void* owner,std::span<const Range> ranges) {
    for(const auto& [begin,end]:ranges)
        if(end<=begin || !registry.Covers(owner,begin,end-begin))return false;
    return true;
}
template<class Registry> void Equivalent(Registry& registry,const void* owner,std::span<const Range> ranges) {
    assert(registry.CoversAll(owner,ranges)==Reference(registry,owner,ranges));
}
int main() {
    using Registry=Native::Parallel::DirectCoherenceRegistry<4,64>;
    Registry registry;int owner1{},owner2{},unknown{};
    registry.Register(&owner1,true);registry.Register(&owner2,false);
    registry.Add(&owner1,100,200,true);registry.Add(&owner1,200,300,true);
    registry.Add(&owner1,350,400,true);registry.Add(&owner1,450,500,false);
    const std::array<Range,3> covered{{{100,300},{120,130},{350,400}}};
    assert(registry.CoversAll(&owner1,covered));Equivalent(registry,&owner1,covered);
    const std::array<Range,2> hole{{{100,300},{290,360}}};assert(!registry.CoversAll(&owner1,hole));Equivalent(registry,&owner1,hole);
    const std::array<Range,1> incoherent{{{450,500}}};assert(!registry.CoversAll(&owner1,incoherent));
    const std::array<Range,1> zero{{{100,100}}}, reversed{{{200,100}}}, overflow{{{UINT64_MAX-4,3}}};
    for(const auto& r:{zero,reversed,overflow}) {assert(!registry.CoversAll(&owner1,r));Equivalent(registry,&owner1,r);}
    const std::array<Range,0> empty{};Equivalent(registry,&owner1,empty);Equivalent(registry,nullptr,empty);Equivalent(registry,&unknown,empty);
    assert(!registry.CoversAll(nullptr,covered));Equivalent(registry,nullptr,covered);
    assert(!registry.CoversAll(&unknown,covered));Equivalent(registry,&unknown,covered);
    assert(!registry.CoversAll(&owner2,covered));Equivalent(registry,&owner2,covered);
    registry.Add(&owner1,150,160,true); // overlap poisons this owner
    assert(!registry.CoversAll(&owner1,covered));Equivalent(registry,&owner1,covered);
    registry.Erase(&owner1);registry.Register(&owner1,true);
    registry.Add(&owner1,UINT64_MAX-16,UINT64_MAX,true);
    const std::array<Range,1> last{{{UINT64_MAX-16,UINT64_MAX}}};assert(registry.CoversAll(&owner1,last));Equivalent(registry,&owner1,last);
    bool busy_result=true;
    registry.TestWithLock([&] {std::thread other([&] {busy_result=registry.CoversAll(&owner1,last);});other.join();});
    assert(!busy_result);

    std::mt19937_64 rng{0x800c0};
    for(unsigned trial=0;trial<4000;++trial) {
        registry.Erase(&owner1);registry.Register(&owner1,true);
        for(U i=0;i<32;++i)if(rng()&1)registry.Add(&owner1,i*32,i*32+16,bool(rng()&1));
        for(unsigned group=0;group<10;++group) {
            std::vector<Range> ranges;
            const auto count=rng()%9;
            for(unsigned i=0;i<count;++i) {
                const U begin=rng()%1100;
                const U end=(rng()%17==0) ? begin : begin+(rng()%80+1);
                ranges.emplace_back(begin,end);
            }
            Equivalent(registry,&owner1,ranges);Equivalent(registry,&owner2,ranges);
            Equivalent(registry,&unknown,ranges);Equivalent(registry,nullptr,ranges);
        }
    }
    Native::Parallel::DirectCoherenceRegistry<1,2> full;int tiny{};full.Register(&tiny,true);
    full.Add(&tiny,1,2,true);full.Add(&tiny,3,4,true);full.Add(&tiny,5,6,true); // slot overflow poisons
    const std::array<Range,1> once{{{1,2}}};assert(!full.CoversAll(&tiny,once));Equivalent(full,&tiny,once);
}
