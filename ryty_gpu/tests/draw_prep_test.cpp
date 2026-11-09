#include "video_core/renderer_vulkan/compile_cpu_batch.h"
#include <cstdlib>
#include "video_core/renderer_vulkan/draw_preparation_options.h"
#include <random>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>
using Range=std::pair<std::uint64_t,std::uint64_t>;
bool overlaps(const std::vector<Range>& v, Range change){
    return std::any_of(v.begin(),v.end(),[&](const Range& r){return change.first<r.second && change.second>r.first;});
}
int main(int argc,char**){
#ifdef _WIN32
    _putenv_s("RYGT_DRAW_PREP_MASK", "0");
    _putenv_s("RYGT_CPU_COMPILE_MASK", "0");
#else
    setenv("RYGT_DRAW_PREP_MASK", "0", 1);
    setenv("RYGT_CPU_COMPILE_MASK", "0", 1);
#endif
    if(Vulkan::DrawPreparationMask()!=15 || Vulkan::CompileCpuBatchMask()!=15)
        throw std::runtime_error("production fixes depend on a debug mask");
    if(argc>1){std::cout<<Vulkan::DrawPreparationMask();return 0;}
    std::mt19937 random(391047);unsigned checks=0;
    for(unsigned trial=0;trial<12000;++trial){
        std::vector<Range> before;
        for(unsigned i=0,n=random()%65;i<n;++i){auto start=random()%4096;before.emplace_back(start,start+1+random()%512);}
        auto after=before;Vulkan::CompactPreparedRanges(after);
        if(after.size()>before.size())throw std::runtime_error("range count grew");
        for(unsigned i=0;i<16;++i){auto begin=random()%4608;Range change{begin,begin+1+random()%128};if(overlaps(before,change)!=overlaps(after,change))throw std::runtime_error("freshness coverage changed");++checks;}
        std::vector<unsigned> values,expected;
        for(unsigned i=0,n=random()%65;i<n;++i){unsigned value=random()%17;values.push_back(value);if(std::find(expected.begin(),expected.end(),value)==expected.end())expected.push_back(value);}
        Vulkan::StableUniquePrepared(values);if(values!=expected)throw std::runtime_error("first occurrence order changed");++checks;
    }
    for(auto v: {std::vector<Range>{{1,1},{0,4}},std::vector<Range>{{5,1},{0,4}}}){auto old=v;Vulkan::CompactPreparedRanges(v);if(v!=old)throw std::runtime_error("malformed or empty range altered");++checks;}
    std::atomic<unsigned> polls{};std::thread a([&]{for(unsigned n=0;n<4096;++n)polls+=Vulkan::PollPreparationDiagnostics();});std::thread b([&]{for(unsigned n=0;n<4096;++n)polls+=Vulkan::PollPreparationDiagnostics();});a.join();b.join();
    if(polls!=(Vulkan::DrawPreparationEnabled(8)?32u:8192u))throw std::runtime_error("thread-local diagnostic polling incorrect");
    std::cout<<"PASS "<<checks<<" range-coverage and stable-order cases, malformed ranges, and per-thread diagnostic polling\n";
}
