// SPDX-License-Identifier: GPL-2.0-or-later
#include "ryty_gpu/native/prepared_sampled_view_proof.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using Handle=std::uintptr_t;
struct OldProof {
    std::size_t slot{},sampled_index{}; Handle handle{}; const void* entry{};
    std::uint64_t owner_epoch{}; bool cube{};
};
bool OldAdd(std::vector<OldProof>& out,std::size_t slot,std::size_t index,Handle handle,
            const void* entry,std::uint64_t epoch,bool cube) {
    if(slot>=128 || !handle || !entry)return false;
    if(out.size()<=slot)out.resize(slot+1);
    out[slot]={slot,index,handle,entry,epoch,cube};return true;
}
template<class Entries> bool OldMatch(const std::vector<OldProof>& proofs,std::size_t slot,
                                      Handle handle,std::uint64_t epoch,const Entries& entries) {
    if(slot>=proofs.size())return false;
    const auto& p=proofs[slot];
    return p.slot==slot && p.handle==handle && p.entry && p.owner_epoch==epoch &&
        p.sampled_index<entries.size() && entries[p.sampled_index]==p.entry;
}
struct HugeEntries {
    std::size_t count{},selected{}; const void* entry{};
    std::size_t size()const{return count;}
    const void* operator[](std::size_t i)const{return i==selected ? entry : nullptr;}
};
int main() {
    using Native::Parallel::AddSampledViewProof;
    using Native::Parallel::MatchSampledViewProof;
    using Proof=Native::Parallel::SampledViewProof<Handle>;
    static_assert(sizeof(void*)!=8 || sizeof(Proof)==32);
    std::printf("sampled proof bytes: old=%zu compact=%zu\n",sizeof(OldProof),sizeof(Proof));
    std::vector<Proof> compact;std::vector<OldProof> old;
    int objects[16]{};std::vector<const void*> entries;
    for(auto& object:objects)entries.push_back(&object);
    std::mt19937_64 rng{0x762b};
    for(unsigned i=0;i<100000;++i) {
        const std::size_t slot=rng()%133,index=rng()%23;
        const Handle handle=rng()%7;const void* entry=(rng()%5) ? entries[rng()%entries.size()] : nullptr;
        const auto epoch=rng()%4;const bool cube=bool(rng()&1);
        assert(AddSampledViewProof(compact,slot,index,handle,entry,epoch,cube)==OldAdd(old,slot,index,handle,entry,epoch,cube));
        for(unsigned n=0;n<4;++n) {
            const auto query_slot=n==0 ? slot : rng()%133;
            const auto query_handle=n==0 ? handle : Handle(rng()%7);
            const auto query_epoch=n==0 ? epoch : rng()%4;
            assert(MatchSampledViewProof(compact,query_slot,query_handle,query_epoch,entries)==
                   OldMatch(old,query_slot,query_handle,query_epoch,entries));
        }
    }
    compact.clear();old.clear();
    assert(AddSampledViewProof(compact,127,UINT32_MAX,Handle{7},entries[0],9,true));
    assert(OldAdd(old,127,UINT32_MAX,Handle{7},entries[0],9,true));
    if constexpr(sizeof(std::size_t)>4) {
        HugeEntries huge{std::size_t{UINT32_MAX}+1,UINT32_MAX,entries[0]};
        assert(MatchSampledViewProof(compact,127,Handle{7},9,huge));
        assert(OldMatch(old,127,Handle{7},9,huge));
        assert(!AddSampledViewProof(compact,127,std::size_t{UINT32_MAX}+1,Handle{7},entries[0],9,true));
        assert(!MatchSampledViewProof(compact,127,Handle{7},9,huge));
    }
    assert(!AddSampledViewProof(compact,128,0,Handle{7},entries[0],9,false));
    assert(!AddSampledViewProof(compact,1,0,Handle{},entries[0],9,false));
    assert(!AddSampledViewProof(compact,1,0,Handle{7},nullptr,9,false));
}
