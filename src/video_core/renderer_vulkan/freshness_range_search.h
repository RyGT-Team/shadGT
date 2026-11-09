#pragma once
#include <algorithm>
#include <span>
#include <utility>
namespace Vulkan {
// Prepared ranges are normally sorted and merged. Other callers keep the exact
// original overlap predicate, including conservative malformed-range behavior.
template<class Address> class FreshnessRangeSearch {
    std::span<const std::pair<Address,Address>> ranges;
    bool ordered{true};
public:
    explicit FreshnessRangeSearch(std::span<const std::pair<Address,Address>> input):ranges{input} {
        for (std::size_t i=0;i<ranges.size();++i) {
            if (ranges[i].first>=ranges[i].second ||
                (i && ranges[i-1].second>ranges[i].first)) { ordered=false;break; }
        }
    }
    bool Intersects(Address begin, Address end) const {
        if (!ordered) {
            for (const auto& range:ranges)
                if (begin<range.second && end>range.first) return true;
            return false;
        }
        const auto it=std::upper_bound(ranges.begin(),ranges.end(),begin,
            [](Address address,const auto& range){return address<range.second;});
        return it!=ranges.end() && it->first<end;
    }
};
}
