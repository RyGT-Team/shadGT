// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <vector>
#include <map>
#include <cassert>
#include <cstdio>
#include <algorithm>
#include <span>
#include "ryty_gpu/native/descriptor_update_batch.h"

using u32=unsigned;using u64=unsigned long long;
namespace vk {
using DescriptorSet=unsigned;using DescriptorBufferInfo=int;using DescriptorImageInfo=int;
enum class DescriptorType {eUniformBuffer,eStorageBuffer,eSampler,eSampledImage,eOther};
}
struct DescriptorSnapshot {
 static bool BufferSliceValid(const int* p,unsigned n,std::span<const int> s){return p && n<=s.size() && p>=s.data() && p+n<=s.data()+s.size();}
 static bool ImageSliceValid(const int* p,unsigned n,std::span<const int> s){return BufferSliceValid(p,n,s);}
};
struct StageRow {unsigned dstSet{},descriptorCount{};const int* pBufferInfo{};const int* pImageInfo{};const void* pNext{};const void* pTexelBufferView{};vk::DescriptorType descriptorType{};bool operator==(const StageRow&) const = default;};
struct StageBatch {enum class Admission {Yes,Capacity,Unsupported};std::array<StageRow,8> writes{};std::array<int,16> buffers{},images{};std::size_t nw{},nb{},ni{};u64 staged_rows{},high_rows{};
#include "descriptor-stage-under-test.inc"
};
struct Write {
    unsigned dstSet,binding,count;const unsigned* info;const void* next;
    bool operator==(const Write&)const=default;
};
int main(){
    std::array<int,3> source{11,22,33};const auto original_source=source;
    std::array<StageRow,4> rows{{{0,1,source.data(),nullptr},{0,2,source.data()+1,nullptr},
                                  {0,1,nullptr,source.data()},{0,2,nullptr,source.data()+1}}};
    const auto original_rows=rows;
    StageBatch batch;
    std::array<std::vector<StageRow>,1> zero{{std::vector<StageRow>{{0,0,source.data(),nullptr}}}};
    assert(batch.Check(zero,1,source,source)==StageBatch::Admission::Unsupported);
    batch.Stage(rows,42);
    assert(rows==original_rows && source==original_source && batch.nw==4 && batch.nb==3 && batch.ni==3);
    for(unsigned i=0;i<4;++i)assert(batch.writes[i].dstSet==42);
    assert(batch.writes[0].pBufferInfo==batch.buffers.data() && batch.writes[1].pBufferInfo==batch.buffers.data()+1);
    assert(batch.writes[2].pImageInfo==batch.images.data() && batch.writes[3].pImageInfo==batch.images.data()+1);
    assert((std::array{batch.buffers[0],batch.buffers[1],batch.buffers[2]}==source));
    assert((std::array{batch.images[0],batch.images[1],batch.images[2]}==source));
    for(std::size_t w=0;w<8;++w) for(std::size_t b=0;b<8;++b) for(std::size_t i=0;i<8;++i)
        for(std::size_t rw=0;rw<8;++rw) for(std::size_t rb=0;rb<8;++rb) for(std::size_t ri=0;ri<8;++ri) {
            bool scan=true;std::size_t x=0,y=0,z=0;
            while(x<w && scan){if(x==rw)scan=false;else ++x;}
            while(y<b && scan){if(y==rb)scan=false;else ++y;}
            while(z<i && scan){if(z==ri)scan=false;else ++z;}
            assert(Native::DescriptorCountsFit(w,b,i,rw,rb,ri)==scan);
        }

    unsigned cases=0;
    for(unsigned code=0;code<256;++code)for(unsigned push=0;push<5;++push)for(unsigned n=1;n<=8;++n){
        std::array<std::vector<Write>,4> groups;
        std::array<unsigned,4> sets{},states{};
        std::array<unsigned,32> infos;for(unsigned i=0;i<infos.size();++i)infos[i]=code*1000+push*100+i;
        std::vector<Write> updates;std::vector<unsigned> expected_events,actual_events;
        std::map<std::pair<unsigned,unsigned>,unsigned> actual_values,expected_values;
        unsigned expected_update_count=0;
        for(unsigned i=0;i<4;++i){
            const auto state=(code>>(i*2))&3;states[i]=state;if(state==0)continue;
            for(unsigned k=0;k<n;++k)groups[i].push_back({0,k%3,1,&infos[i*8+k],&infos[31]});
            if(i==push){expected_events.push_back(100+i);continue;}
            if(state==3)continue;sets[i]=17+i;expected_events.push_back(i);
            if(state==2){
                Native::AppendDescriptorUpdates(groups[i],sets[i],updates);++expected_update_count;
                for(const auto& write:groups[i])expected_values[{sets[i],write.binding}]=*write.info;
            }
        }
        const auto original=groups;unsigned calls=0;
        Native::CommitDescriptorBatch(updates,sets,push,groups,
            [&](const auto& all){
                assert(actual_events.empty());++calls;
                for(const auto& write:all){assert(write.next==&infos[31]);actual_values[{write.dstSet,write.binding}]=*write.info;}
            },
            [&](unsigned i,const auto& writes){assert(writes==groups[i]);actual_events.push_back(100+i);},
            [&](unsigned i,unsigned set){assert(set==sets[i]);assert(actual_values==expected_values);actual_events.push_back(i);});
        assert(calls==(expected_update_count!=0));assert(actual_values==expected_values);
        assert(actual_events==expected_events);assert(groups==original);++cases;
    }
    std::printf("PASS %u descriptor plans: hits/misses/failed allocations, push positions, overlapping writes, pointer lifetime and original bind order\n",cases);
}
