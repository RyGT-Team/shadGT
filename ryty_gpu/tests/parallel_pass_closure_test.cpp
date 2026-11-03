#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
using u32=std::uint32_t;using u64=std::uint64_t;
#include "pass-closure-under-test.inc"
struct MockParallel {bool active;};
bool EnvironmentSafe(bool active,bool fast_race,bool fast_trace,bool prohibited_clear,
                     bool has_feedback,bool scale_composition,bool native_fsr_composition) {
 MockParallel p{active};auto* parallel=&p;std::vector<int> feedback;if(has_feedback)feedback.push_back(1);
#include "inline-safe-under-test.inc"
 return inline_safe;
}
struct Pass {int target{};std::vector<int> draws;};
#include "emit-pass-under-test.inc"
int main(){
 assert(EnvironmentSafe(true,true,false,false,false,false,false));
 assert(!EnvironmentSafe(false,true,false,false,false,false,false));
 assert(!EnvironmentSafe(true,false,false,false,false,false,false));
 assert(!EnvironmentSafe(true,true,true,false,false,false,false));
 assert(!EnvironmentSafe(true,true,false,true,false,false,false));
 assert(!EnvironmentSafe(true,true,false,false,true,false,false));
 assert(!EnvironmentSafe(true,true,false,false,false,true,false));
 assert(!EnvironmentSafe(true,true,false,false,false,false,true));
 // Every safety condition is required; 0 and 8 cannot inline.
 assert(CanInlineContinuation(1,false,false,true,true,true,true,true,true));
 assert(CanInlineContinuation(7,false,false,true,true,true,true,true,true));
 assert(!CanInlineContinuation(0,false,false,true,true,true,true,true,true));
 assert(!CanInlineContinuation(8,false,false,true,true,true,true,true,true));
 assert(!CanInlineContinuation(1,true,false,true,true,true,true,true,true));
 assert(!CanInlineContinuation(1,false,true,true,true,true,true,true,true));
 assert(!CanInlineContinuation(1,false,false,false,true,true,true,true,true));
 assert(!CanInlineContinuation(1,false,false,true,false,true,true,true,true));
 assert(!CanInlineContinuation(1,false,false,true,true,false,true,true,true));
 assert(!CanInlineContinuation(1,false,false,true,true,true,false,true,true));
 assert(!CanInlineContinuation(1,false,false,true,true,true,true,false,true));
 assert(!CanInlineContinuation(1,false,false,true,true,true,true,true,false));
 Pass recorded{0,{1,2}};std::vector<int> sequence;
 auto begin=[&](auto){sequence.push_back(10);};auto draw=[&](auto n){sequence.push_back(n);};
 bool leave_open=true;
 EmitWholePass(recorded,begin,draw,[&]{if(!leave_open)sequence.push_back(20);});
 sequence.push_back(3);sequence.push_back(20);sequence.push_back(30); // serial draw, later end, barrier
 assert((sequence==std::vector<int>{10,1,2,3,20,30}));
 sequence.clear();leave_open=false;EmitWholePass(recorded,begin,draw,[&]{if(!leave_open)sequence.push_back(20);});
 sequence.push_back(30);assert((sequence==std::vector<int>{10,1,2,20,30}));
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
