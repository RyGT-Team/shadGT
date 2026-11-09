#pragma once
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <utility>
#include <vector>
namespace Vulkan {
// Preparation fixes are always enabled; stale diagnostic masks cannot disable them.
constexpr unsigned DrawPreparationMask() { return 15u; }
inline bool DrawPreparationEnabled(unsigned bit){return (DrawPreparationMask()&bit)!=0;}
inline bool PollPreparationDiagnostics(){
    if(!DrawPreparationEnabled(8))return true;
    static thread_local unsigned calls{};return (calls++&255u)==0;
}
// Only the union matters to freshness checks. Preserve malformed/empty inputs
// conservatively, and never alter descriptor offsets or descriptor order.
template<class Address> void CompactPreparedRanges(std::vector<std::pair<Address,Address>>& ranges){
    if(ranges.size()<2)return;
    for(const auto& r:ranges)if(r.first>=r.second)return;
    std::sort(ranges.begin(),ranges.end());std::size_t used=0;
    for(const auto& r:ranges){
        if(used && r.first<=ranges[used-1].second)ranges[used-1].second=std::max(r.second,ranges[used-1].second);
        else ranges[used++]=r;
    }
    ranges.resize(used);
}
template<class Sequence> void StableUniquePrepared(Sequence& values){
    std::size_t used=0;
    for(const auto& value:values){
        if(std::find(values.begin(),values.begin()+used,value)==values.begin()+used)values[used++]=value;
    }
    values.resize(used);
}
}
