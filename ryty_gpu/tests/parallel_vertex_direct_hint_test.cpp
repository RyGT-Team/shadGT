#include <cassert>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

using u64 = std::uint64_t;
using u32 = std::uint32_t;
namespace VideoCore {
struct Buffer { u64 handle; u64 Handle() const { return handle; } };
struct Direct {
    std::vector<std::pair<u64, u64>> calls;
    bool coherent = true, available = true;
    u64 mapped_address = 0, handle = 0, offset = 0;
    u64 part_offset = 0, length_override = 0;
    unsigned parts = 1;
    template<class Ready, class Fn> bool TryForEachPart(u64 address, u64 bytes, Ready ready, Fn fn) {
        calls.emplace_back(address, bytes);
        if (!available || address != mapped_address || !ready()) return false;
        for (unsigned i = 0; i < parts; ++i)
            fn(Buffer{handle}, offset, part_offset, length_override ? length_override : bytes);
        return true;
    }
};
bool ParallelDirectViewCoherent(Direct* direct, u64, u64) { return direct->coherent; }
}
struct Prepared { std::vector<std::pair<u64,u64>> in_place; };
struct Stats { u64 vertex_direct_attempts{}, vertex_direct_matches{}; };
static bool check(Prepared* prepared, VideoCore::Direct* direct, Stats* parallel,
                  std::size_t& vertex_direct_hint, u64 handle, u64 offset, u64 size) {
    bool owned = false;
#include "vertex-direct-hint-under-test.inc"
    return owned;
}
static bool original(Prepared& prepared, VideoCore::Direct& direct, u64 handle, u64 offset, u64 size) {
    for(const auto& [address,bytes] : prepared.in_place) {
        if(bytes!=size) continue;
        u32 parts=0;bool exact=false;
        const bool covered=direct.TryForEachPart(address,bytes,
            [&] {return VideoCore::ParallelDirectViewCoherent(&direct,address,bytes);},
            [&](const VideoCore::Buffer& source,u64 source_offset,u64 part_offset,u64 length) {
                ++parts;exact=source.Handle()==handle && source_offset==offset && part_offset==0 && length==bytes;
            });
        if(covered && parts==1 && exact) return true;
    }
    return false;
}
int main() {
    Prepared p{{{10, 8}, {20, 8}, {30, 8}, {40, 16}}};
    VideoCore::Direct direct;
    Stats stats;
    std::size_t hint = 0;
    direct.mapped_address=10;direct.handle=1;direct.offset=3;
    assert(check(&p,&direct,&stats,hint,1,3,8) && hint==1);
    direct.calls.clear();direct.mapped_address=20;
    assert(check(&p,&direct,&stats,hint,1,3,8) && direct.calls.size()==1 && hint==2);
    direct.calls.clear();direct.mapped_address=10;
    assert(check(&p,&direct,&stats,hint,1,3,8) && direct.calls.size()==2 && hint==1);
    direct.coherent=false;
    assert(!check(&p,&direct,&stats,hint,1,3,8));
    direct.coherent=true;direct.available=false;
    assert(!check(&p,&direct,&stats,hint,1,3,8));
    direct.available=true;direct.parts=2;
    assert(!check(&p,&direct,&stats,hint,1,3,8));
    direct.parts=1;direct.offset=4;
    assert(!check(&p,&direct,&stats,hint,1,3,8));
    direct.offset=3;direct.part_offset=1;
    assert(!check(&p,&direct,&stats,hint,1,3,8));
    direct.part_offset=0;direct.length_override=7;
    assert(!check(&p,&direct,&stats,hint,1,3,8));
    direct.length_override=0;
    direct.offset=3;direct.handle=2;
    assert(!check(&p,&direct,&stats,hint,1,3,8));
    p.in_place.clear();
    assert(!check(&p,&direct,&stats,hint,1,3,8));

    Prepared ordered;
    for(u64 i=0;i<32;++i) ordered.in_place.emplace_back(100+i,8);
    hint=0;direct={};direct.handle=1;direct.offset=3;
    std::size_t old_calls=0,new_calls=0;
    for(u64 i=0;i<32;++i) {
        direct.mapped_address=100+i;direct.calls.clear();
        assert(original(ordered,direct,1,3,8));old_calls+=direct.calls.size();
        direct.calls.clear();
        assert(check(&ordered,&direct,&stats,hint,1,3,8));new_calls+=direct.calls.size();
    }
    assert(old_calls==528 && new_calls==32);

    std::mt19937 rng{734};
    for(unsigned trial=0;trial<5000;++trial) {
        Prepared input;
        const unsigned count=rng()%33;
        for(unsigned i=0;i<count;++i) input.in_place.emplace_back(rng()%16,1+rng()%8);
        VideoCore::Direct old_direct;
        old_direct.mapped_address=rng()%18;old_direct.handle=rng()%3;
        old_direct.offset=rng()%4;old_direct.parts=rng()%3;
        old_direct.coherent=(rng()%5)!=0;old_direct.available=(rng()%5)!=0;
        auto new_direct=old_direct;
        const u64 h=rng()%3,off=rng()%4,size=1+rng()%8;
        std::size_t candidate_hint=count ? rng()%count : 0;
        assert(original(input,old_direct,h,off,size)==
               check(&input,&new_direct,&stats,candidate_hint,h,off,size));
    }
}
