#include "../native/aligned_erase.h"
#include <cassert>
#include <random>
#include <unordered_map>
#include <unordered_set>
using U=std::uint64_t;
int main() {
    std::mt19937_64 random{411};
    for (int n=0;n<5000;++n) {
        std::unordered_map<U,int> reference, candidate;
        std::unordered_set<U> ref_set, candidate_set;
        for (int i=0;i<200;++i) { U key=(random()%512)*4096; reference[key]=i; ref_set.insert(key); }
        reference[UINT64_MAX & ~U{4095}]=7; ref_set.insert(UINT64_MAX & ~U{4095});
        candidate=reference; candidate_set=ref_set;
        U address=(random()%520)*4096 + random()%4096, size=random()%65537;
        if (n%101==0) address=UINT64_MAX-random()%8192;
        if (n%103==0) size=0;
        const auto start_pred=[&](const auto& entry){return entry.first>=address && entry.first<address+size;};
        const auto overlap_pred=[&](U page){return page<address+size && page+4096>address;};
        std::erase_if(reference,start_pred);
        Native::EraseAligned<4096>(candidate,address,size,false,true,start_pred);
        auto map_overlap=reference;
        auto map_expected=reference;
        const auto map_pred=[&](const auto& entry){return entry.first<address+size && entry.first+4096>address;};
        std::erase_if(map_expected,map_pred);
        Native::EraseAligned<4096>(map_overlap,address,size,true,true,map_pred);
        assert(map_expected==map_overlap);
        std::erase_if(ref_set,overlap_pred);
        Native::EraseAligned<4096>(candidate_set,address,size,true,true,overlap_pred);
        assert(reference==candidate && ref_set==candidate_set);
    }
    std::unordered_set<U> large;
    for (U i=0;i<4096;++i) large.insert(i*4096);
    unsigned checks=0;
    Native::EraseAligned<4096>(large,4096*100,4096,true,true,[&](U page) { ++checks; return page<4096*101 && page+4096>4096*100; });
    assert(checks==1 && !large.contains(4096*100));
    auto disabled=large; checks=0;
    Native::EraseAligned<4096>(disabled,0,4096,true,false,[&](U page) { ++checks; return page<4096; });
    assert(checks==large.size());
    for (int n=0;n<5000;++n) {
        std::unordered_map<U,int> original;
        for (int i=0;i<200;++i) original[(random()%512)*65536]=i;
        original[UINT64_MAX & ~U{65535}]=1;
        auto indexed=original;
        U address=(random()%520)*65536 + random()%65536, size=random()%262145;
        if (n%101==0) address=UINT64_MAX-random()%131072;
        if (n%103==0) size=0;
        const auto predicate=[&](const auto& e){return e.first<address+size && e.first+65536>address;};
        std::erase_if(original,predicate);
        Native::EraseAligned<65536>(indexed,address,size,true,true,predicate);
        assert(indexed==original);
    }
    std::unordered_set<U> edge{0,4096,8192,12288,16384,20480,24576,28672,32768};
    Native::EraseAligned<4096>(edge,4096,4096,true,true,[](U p){return p<8192 && p+4096>4096;});
    assert(!edge.contains(4096) && edge.contains(0) && edge.contains(8192));
}
