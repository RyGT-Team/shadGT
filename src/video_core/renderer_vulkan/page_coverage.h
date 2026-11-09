// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace Vulkan {
// Diagnostic counts only. Accumulate ranges instead of allocating a hash node
// for every page on a sampled frame; retain vector storage between samples.
class PageCoverage {
    using U=std::uint64_t;
public:
    void Add(U address,U bytes){
        if(!bytes)return;
        const U last=address>std::numeric_limits<U>::max()-(bytes-1)
            ? std::numeric_limits<U>::max():address+bytes-1;
        const U begin=address>>16,end=(last>>16)+1;
        if(!ranges.empty() && begin<=ranges.back().second && end>=ranges.back().first){
            ranges.back().first=std::min(begin,ranges.back().first);
            ranges.back().second=std::max(end,ranges.back().second);
        }else ranges.emplace_back(begin,end);
    }
    U TakePageCount(){
        if(ranges.empty())return 0;
        std::sort(ranges.begin(),ranges.end());
        U count=0,begin=ranges.front().first,end=ranges.front().second;
        for(size_t i=1;i<ranges.size();++i){
            if(ranges[i].first<=end)end=std::max(end,ranges[i].second);
            else {count+=end-begin;begin=ranges[i].first;end=ranges[i].second;}
        }
        count+=end-begin;ranges.clear();return count;
    }
private:std::vector<std::pair<U,U>> ranges;
};
}
