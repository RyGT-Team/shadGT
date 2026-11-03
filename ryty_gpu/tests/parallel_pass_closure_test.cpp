#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
using u32=std::uint32_t;using u64=std::uint64_t;
#include "pass-closure-under-test.inc"
int main(){
 PassClosureStats stats;assert(stats.origin==2);
 const std::array<std::size_t,13> n{0,1,2,3,4,7,8,31,32,255,256,257,1};
 const std::array<std::size_t,13> bucket{0,1,2,2,3,3,4,4,5,5,6,6,1};
 for(std::size_t i=0;i<n.size();++i){stats.origin=i%3;stats.Close(n[i],i%2==0,i==0?0:CloseFast);}
 std::array<std::array<std::array<u64,7>,2>,3> expected{};
 for(std::size_t i=0;i<n.size();++i)++expected[i%3][i%2?1:0][bucket[i]];
 assert(stats.sizes==expected && stats.reasons[0][0]==1);
 stats.origin=1;stats.Close(5,true,CloseTarget|CloseFull);assert(stats.reasons[1][4]==1 && stats.reasons[1][5]==1);
 std::vector<int> pass(8),spare(1);const auto before_transfer=pass.size();pass.swap(spare);
 stats.Close(before_transfer,true,CloseMissingSets);assert(stats.sizes[1][0][4]==expected[1][0][4]+1);
 assert(stats.reasons[1][6]==1);++stats.empty_aborts[1];
 stats.sizes={};stats.reasons={};stats.empty_aborts={};assert(stats.origin==1);
 for(const auto& o:stats.sizes)for(const auto& q:o)for(auto value:q)assert(value==0);
 for(const auto& o:stats.reasons)for(auto value:o)assert(value==0);
 assert(stats.empty_aborts[1]==0);
}
