#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace Native::Parallel {
template<class Handle> struct SampledViewProof {
    std::size_t slot{},sampled_index{}; Handle handle{}; const void* entry{}; std::uint64_t owner_epoch{}; bool cube{};
};
template<class Handle> bool AddSampledViewProof(std::vector<SampledViewProof<Handle>>& out,
    std::size_t slot,std::size_t sampled_index,Handle handle,const void* entry,std::uint64_t owner_epoch,bool cube) noexcept {
    if(slot>=128 || !handle || !entry)return false;
    try {if(out.size()<=slot)out.resize(slot+1);out[slot]={slot,sampled_index,handle,entry,owner_epoch,cube};return true;} catch(...) {return false;}
}
template<class Handle,class Entries> bool MatchSampledViewProof(const std::vector<SampledViewProof<Handle>>& proofs,
    std::size_t slot,Handle handle,std::uint64_t owner_epoch,const Entries& entries) noexcept {
    if(slot>=proofs.size())return false;
    const auto& p=proofs[slot];
    return p.slot==slot && p.handle==handle && p.entry && p.owner_epoch==owner_epoch &&
        p.sampled_index<entries.size() && entries[p.sampled_index]==p.entry;
}
template<class Handle> struct SampledMemoProof {
    const void* owner{};const void* memo{};const void* entry{};
    std::uint64_t owner_epoch{},image_generation{},unmap_generation{};
    std::array<std::uint64_t,4> sharp{};std::uint32_t flags{};bool cube{};
    std::array<Handle,4> views{};
    bool Matches(const void* o,const void* m,const void* e,std::uint64_t epoch,
        std::uint64_t image,std::uint64_t unmap,const std::array<std::uint64_t,4>& s,
        std::uint32_t f,bool c,const std::array<Handle,4>& v) const noexcept {
        return owner==o && memo==m && entry==e && e && owner_epoch==epoch && image_generation==image &&
            unmap_generation==unmap && sharp==s && flags==f && cube==c && views==v;
    }
};
}
