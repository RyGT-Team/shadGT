#include "ryty_gpu/native/cpu_stack_registry.h"
#include <cassert>
#include <atomic>
#include <thread>
#include <random>
#include <vector>
#include <utility>
int main() {
    // Compare the merged page coverage against the original interval rule while
    // adding and removing nested, duplicate, overlapping and bridging ranges.
    Native::CpuStackRegistry fuzz;
    std::vector<std::pair<uint64_t,uint64_t>> live;
    std::mt19937_64 random{12345};
    for (unsigned step = 0; step < 4000; ++step) {
        if (live.empty() || (random() & 1)) {
            auto begin = (random() % 64) * 997, size = 1 + random() % 10000;
            if (!live.empty() && (random() % 5 == 0)) { auto previous=live[random()%live.size()]; begin=previous.first; size=previous.second; }
            assert(fuzz.Register(begin, size, [] {})); live.emplace_back(begin,size);
        } else {
            const auto i = random() % live.size(); auto [begin,size]=live[i];
            fuzz.Unregister(begin,size); live.erase(live.begin()+i);
        }
        Native::CpuStackRegistry::Lease lease{fuzz};
        for (unsigned q=0; q<8; ++q) {
            const auto begin=random()%75000, end=begin+1+random()%5000;
            const bool expected=std::none_of(live.begin(),live.end(),[&](auto x){
                const auto lo=x.first&~uint64_t{4095}, hi=(x.first+x.second+4095)&~uint64_t{4095};
                return lo<end && begin<hi;
            });
            assert(lease.Allowed(begin,end)==expected);
        }
    }
    Native::CpuStackRegistry r;
    assert(!r.Register(0, 0, [] {}));
    assert(!r.Register(~0ull - 1, 2, [] {}));
    assert(!r.Register(0x1001, 16, [] { throw 1; }));
    { Native::CpuStackRegistry::Lease l{r}; assert(l.Allowed(0x1000,0x2000)); }
    assert(r.Register(0x1001, 16, [] {}));
    assert(r.Register(0x1001, 16, [] {}));
    { Native::CpuStackRegistry::Lease l{r}; assert(!l.Allowed(0x1fff,0x2000)); assert(l.Allowed(0x2000,0x3000)); }
    r.Unregister(0x1001,16);
    { Native::CpuStackRegistry::Lease l{r}; assert(!l.Allowed(0x1000,0x1001)); }
    r.Unregister(0x1001,16);
    std::atomic<bool> entered{}, release{}, registered{};
    std::thread writer([&] { registered=r.Register(0x4000,4096,[&]{ entered=true; while(!release.load()) std::this_thread::yield(); }); });
    while (!entered.load()) std::this_thread::yield();
    { Native::CpuStackRegistry::Lease l{r}; assert(!l.Allowed(0x9000,0xa000)); }
    release=true; writer.join(); assert(registered);
    { Native::CpuStackRegistry::Lease l{r}; assert(!l.Allowed(0x4000,0x5000)); assert(l.Allowed(0x5000,0x6000)); }
}
