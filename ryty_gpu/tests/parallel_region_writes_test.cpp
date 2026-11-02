#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <map>
#include <random>
#include <vector>
using VAddr = std::uint64_t;
struct Region { VAddr begin, end; std::vector<VAddr> dirty_pages; bool dirty_sorted = true; };
using Map = std::map<VAddr, Region>;
static void current(Map& regions, VAddr begin, VAddr end) {
#include "region-writes-under-test.inc"
}
static void old(Map& regions, VAddr begin, VAddr end) {
    for (auto& [base, region] : regions) {
        if (region.begin < end && region.end > begin) {
            for (VAddr page = std::max(begin, region.begin); page < std::min(end, region.end); page += 4096)
                region.dirty_pages.push_back(page);
            region.dirty_sorted = false;
        }
    }
}
static std::vector<VAddr> normalize(const Region& r) {
    auto v = r.dirty_pages;
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}
static void compare(const Map& a, const Map& b) {
    assert(a.size() == b.size());
    auto bi = b.begin();
    for (auto const& [key, r] : a) {
        assert(key == bi->first);
        assert(normalize(r) == normalize(bi->second));
        if (r.dirty_sorted) {
            assert(std::is_sorted(r.dirty_pages.begin(), r.dirty_pages.end()));
            assert(std::adjacent_find(r.dirty_pages.begin(), r.dirty_pages.end()) == r.dirty_pages.end());
        }
        ++bi;
    }
}
static void refresh(Map& m, VAddr begin, VAddr end) {
    for (auto& [key,r] : m) {
        r.dirty_pages = normalize(r); r.dirty_sorted = true;
        auto first = std::lower_bound(r.dirty_pages.begin(),r.dirty_pages.end(),begin);
        auto last = std::lower_bound(first,r.dirty_pages.end(),end);
        r.dirty_pages.erase(first,last);
    }
}
int main() {
    std::mt19937_64 gen(0x081617);
    for (int trial = 0; trial < 4000; ++trial) {
        Map a;
        VAddr cursor = 0;
        for (int i=0,n=static_cast<int>(gen()%24); i<n; ++i) {
            cursor += (gen()%4)*4096;
            VAddr end = cursor+(1+gen()%8)*4096;
            Region r{cursor,end,{},true};
            int count = static_cast<int>(gen()%7);
            for(int j=0;j<count;++j) r.dirty_pages.push_back(cursor+(gen()%((end-cursor)/4096))*4096);
            r.dirty_sorted = (gen()%2) != 0;
            if(r.dirty_sorted) r.dirty_pages = normalize(r);
            a.emplace(cursor,std::move(r)); cursor=end;
        }
        Map b=a;
        for(int i=0;i<80;++i) {
            VAddr begin=(gen()%200)*4096, end=(gen()%200)*4096;
            if(i%4==0) end=begin;
            if(i%4==1 && begin>end) std::swap(begin,end);
            if(i%11==0 && !a.empty()) { auto it=std::next(a.begin(),gen()%a.size()); begin=it->second.begin; end=it->second.end; }
            current(a,begin,end); old(b,begin,end); compare(a,b);
            if(i%13==0) { VAddr lo=std::min(begin,end), hi=std::max(begin,end); refresh(a,lo,hi); refresh(b,lo,hi); compare(a,b); }
            if(i%31==0 && a.size()>1) {
                auto it=a.begin(), next=std::next(it); Region merged{it->second.begin,next->second.end,it->second.dirty_pages,false};
                merged.dirty_pages.insert(merged.dirty_pages.end(),next->second.dirty_pages.begin(),next->second.dirty_pages.end());
                auto itb=b.begin(), nb=std::next(itb); Region mergedb{itb->second.begin,nb->second.end,itb->second.dirty_pages,false};
                mergedb.dirty_pages.insert(mergedb.dirty_pages.end(),nb->second.dirty_pages.begin(),nb->second.dirty_pages.end());
                a.erase(it,next==a.end()?next:std::next(next)); b.erase(itb,std::next(nb));
                a.emplace(merged.begin,std::move(merged)); b.emplace(mergedb.begin,std::move(mergedb)); compare(a,b);
            }
        }
    }
}
