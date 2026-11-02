// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>
using u64=std::uint64_t;using VAddr=u64;
template<class = void> struct ImportedPageCoverage {static constexpr u64 Limit=1024;};
struct Buffer {int id;};
struct DirectMemory {
    struct View {u64 begin,end;std::shared_ptr<Buffer> wrapper;};
    std::map<u64,View> views;mutable std::mutex mutex;
    bool TryForEachPart(VAddr,u64,const std::function<bool()>&,const std::function<void(const Buffer&,u64,u64,u64)>&) const;
    bool Original(VAddr address,u64 size,const std::function<bool()>& ready,const std::function<void(const Buffer&,u64,u64,u64)>& fn) const {
        if(address>ImportedPageCoverage<>::Limit || size==0 || size>ImportedPageCoverage<>::Limit-address)return false;
        std::unique_lock lock{mutex,std::try_to_lock};if(!lock.owns_lock())return false;
        const VAddr end=address+size;
        for(VAddr cursor=address;cursor<end;) {
            const auto it=views.upper_bound(cursor);if(it==views.begin())return false;
            const View& view=std::prev(it)->second;
            if(cursor<view.begin || cursor>=view.end || !view.wrapper)return false;
            cursor=std::min(end,view.end);
        }
        if(!ready())return false;
        for(VAddr cursor=address;cursor<end;) {
            const View& view=std::prev(views.upper_bound(cursor))->second;
            const u64 bytes=std::min(end,view.end)-cursor;
            fn(*view.wrapper,cursor-view.begin,cursor-address,bytes);cursor+=bytes;
        }
        return true;
    }
};
#include "direct-try-parts-under-test.inc"
using Trace=std::vector<std::tuple<int,u64,u64,u64>>;
struct Result {bool result{},threw{};int ready{};Trace trace;bool operator==(const Result&)const=default;};
static Result Run(DirectMemory& d,bool original,u64 address,u64 size,bool ready,int throw_at=0) {
    Result r;
    try {
        const auto check=[&] {++r.ready;return ready;};
        const auto visit=[&](const Buffer& b,u64 offset,u64 relative,u64 bytes) {
            r.trace.emplace_back(b.id,offset,relative,bytes);
            if(throw_at && int(r.trace.size())==throw_at)throw std::runtime_error("callback");
        };
        r.result=original?d.Original(address,size,check,visit):d.TryForEachPart(address,size,check,visit);
    } catch(const std::runtime_error&) {r.threw=true;}
    return r;
}
int main() {
    DirectMemory d;auto a=std::make_shared<Buffer>(Buffer{1}),b=std::make_shared<Buffer>(Buffer{2});
    d.views={{10,{10,20,a}},{20,{20,30,b}},{40,{40,50,a}},{50,{50,60,{}}},{1010,{1010,1024,b}}};
    for(u64 address: {u64{0},u64{9},u64{10},u64{11},u64{19},u64{20},u64{21},u64{29},u64{30},u64{39},u64{40},u64{49},u64{50},u64{59},u64{60},u64{1010},u64{1023},u64{1024},UINT64_MAX})
      for(u64 size: {u64{0},u64{1},u64{2},u64{9},u64{10},u64{11},u64{20},u64{21},UINT64_MAX})
       for(bool ready: {false,true})for(int throws: {0,1,2})
        assert(Run(d,false,address,size,ready,throws)==Run(d,true,address,size,ready,throws));
    auto single=Run(d,false,11,3,true);assert(single.result && single.ready==1 && single.trace==Trace({{1,1,0,3}}));
    auto multi=Run(d,false,15,10,true);assert(multi.result && multi.ready==1 && multi.trace==Trace({{1,5,0,5},{2,0,5,5}}));
    auto gap=Run(d,false,25,20,true);assert(!gap.result && gap.ready==0 && gap.trace.empty());
    auto not_ready=Run(d,false,11,3,false);assert(!not_ready.result && not_ready.ready==1 && not_ready.trace.empty());
    int ready_calls=0, visits=0;
    for(bool old: {false,true}) {
        try {
            const auto ready_throw=[&]() -> bool {++ready_calls;throw std::runtime_error("ready");};
            const auto visit=[&](const Buffer&,u64,u64,u64) {++visits;};
            if(old)d.Original(11,3,ready_throw,visit);else d.TryForEachPart(11,3,ready_throw,visit);
            assert(false);
        } catch(const std::runtime_error&) {}
    }
    assert(ready_calls==2 && visits==0);
    std::unique_lock held{d.mutex};Result busy;
    std::thread worker([&] {busy=Run(d,false,11,3,true);});worker.join();
    assert(!busy.result && !busy.threw && busy.ready==0 && busy.trace.empty());
}
