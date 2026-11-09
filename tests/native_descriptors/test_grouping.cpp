// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/descriptor_write_groups.h"
#include <vector>
#include <stdexcept>
#include <iostream>
struct Write { unsigned value; bool operator==(const Write&) const = default; };
struct IndirectWrite { const unsigned* info; unsigned binding; };
int main() {
    unsigned cases=0, borrowed=0;
    for(unsigned length=0;length<=7;++length) {
        unsigned combinations=1;for(unsigned i=0;i<length;++i) combinations*=3;
        for(unsigned pattern=0;pattern<combinations;++pattern) {
            std::vector<Write> writes;std::vector<std::uint8_t> sets;
            unsigned v=pattern;for(unsigned i=0;i<length;++i){writes.push_back({i});sets.push_back(v%3);v/=3;}
            for(unsigned tags=0;tags<=length;++tags) {
                std::array<std::vector<Write>,4> scratch,expected;
                for(unsigned i=0;i<length;++i)expected[i<tags?sets[i]:0].push_back(writes[i]);
                auto actual=Vulkan::GroupDescriptorWrites<Write>(writes,{sets.data(),tags},scratch);
                for(unsigned set=0;set<4;++set) {
                    if(!std::equal(actual[set].begin(),actual[set].end(),expected[set].begin(),expected[set].end()))throw std::runtime_error("group values/order changed");
                    if(!actual[set].empty()&&actual[set].data()>=writes.data()&&actual[set].data()<writes.data()+writes.size())++borrowed;
                }
                ++cases;
            }
        }
    }
    if(!borrowed)throw std::runtime_error("no borrowed ranges");
    // Prepared descriptor copies retain pointers into stable per-draw buffer/image
    // slots. Ordered commit replaces those slots before recording.
    for (bool interleaved : {false,true}) {
        std::array<unsigned,4> info{1,2,3,4};
        std::array<IndirectWrite,4> writes{{{&info[0],0},{&info[1],1},{&info[2],2},{&info[3],3}}};
        std::array<std::uint8_t,4> tags = interleaved ? std::array<std::uint8_t,4>{0,1,0,1}
                                                    : std::array<std::uint8_t,4>{0,0,1,1};
        std::array<std::vector<IndirectWrite>,4> storage;
        auto groups=Vulkan::GroupDescriptorWrites<IndirectWrite>(writes,tags,storage);
        for(auto& value:info)value+=100;
        for(const auto& group:groups)for(const auto& write:group)
            if(*write.info!=101+write.binding)throw std::runtime_error("prepared buffer update lost");
        // Reuse the same scratch with a different set layout: no old writes survive.
        tags.fill(3);groups=Vulkan::GroupDescriptorWrites<IndirectWrite>(writes,tags,storage);
        if(!groups[0].empty()||!groups[1].empty()||groups[3].size()!=4)throw std::runtime_error("stale pooled groups");
    }
    std::cout<<"PASS "<<cases<<" grouping cases, "<<borrowed<<" borrowed ranges\n";
}
