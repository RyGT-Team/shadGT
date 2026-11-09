// CPU contract: production publication and traversal with mocked draw emission.
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <vector>
#include "draw_publication.h"
using namespace std::chrono_literals;
namespace Native::Parallel {
struct Draw {int value{};int checksum{};};
struct Pass {int target=42;bool extended_verified=true;std::vector<Draw> draws;};
bool ValidateDraw(const Draw& draw,bool){return draw.value>0 && draw.checksum==draw.value*13;}
template<class Begin,class Emit,class End>
void EmitWholePass(const Pass& pass,Begin&& begin,Emit&& emit,End&& end){
    begin(pass.target);for(const auto& draw:pass.draws)emit(draw);end();
}
#include "stream-traversal.inc"
}
using namespace Native::Parallel;
int main(){
    // Stable element addresses, complete slots, incremental consumption before
    // sealing, then a metadata-only vector transfer to the retained job.
    for(int repeat=0;repeat<100;++repeat){
        Pass source,retained;source.draws.reserve(repeat%2?64:256);
        for(int i=1;i<=8;++i)source.draws.push_back({i,i*13});
        const Draw* storage=source.draws.data();DrawPublication publication;
        assert(publication.Publish(8));
        std::atomic<std::size_t> consumed{};
        std::size_t early_draws=0;int begins=0,ends=0;std::vector<int> emitted;
        auto worker=std::async(std::launch::async,[&]{
            PassTraversal traversal{retained,&publication,storage
#if !defined(NDEBUG)
                ,&early_draws
#endif
            };
            EmitWholePass(traversal,[&](int target){assert(target==42);++begins;},
                [&](const Draw& draw){emitted.push_back(draw.value);consumed.store(emitted.size(),std::memory_order_release);consumed.notify_one();},
                [&]{++ends;});
            return traversal.valid;
        });
        auto observed=consumed.load(std::memory_order_acquire);
        while(observed<8){consumed.wait(observed);observed=consumed.load(std::memory_order_acquire);}
        assert(!publication.Load().sealed);
        for(int i=9;i<=64;++i){source.draws.push_back({i,i*13});assert(publication.Publish(i));}
        retained.draws.swap(source.draws);assert(retained.draws.data()==storage && source.draws.empty());
        publication.Seal();
        assert(worker.wait_for(2s)==std::future_status::ready && worker.get());
        assert(begins==1 && ends==1 && emitted.size()==64);
        for(int i=0;i<64;++i)assert(emitted[i]==i+1);
#if !defined(NDEBUG)
        assert(early_draws>=7);
#endif
        assert(!publication.Publish(65) && !publication.Publish(64));
    }
    {DrawPublication publication;assert(publication.Publish(8));assert(!publication.Publish(7));
     assert(!publication.Publish(65));assert(publication.Load().count==8);}
    {DrawPublication publication;
     auto waiter=std::async(std::launch::async,[&]{return publication.Wait(0);});
     publication.Seal();assert(waiter.wait_for(2s)==std::future_status::ready);
     const auto ended=waiter.get();assert(ended.sealed && ended.count==0);}
    // An invalid newly published packet aborts recording after the valid prefix;
    // the caller can discard the unsubmitted command buffer and recover later.
    {Pass header;std::vector<Draw> storage{{1,13},{2,0}};DrawPublication publication;
     assert(publication.Publish(2));publication.Seal();int emitted=0,ends=0;
     std::size_t early=0;PassTraversal traversal{header,&publication,storage.data()
#if !defined(NDEBUG)
        ,&early
#endif
     };
     EmitWholePass(traversal,[](int){},[&](const Draw&){++emitted;},[&]{++ends;});
     assert(!traversal.valid && emitted==1 && ends==1);}
}
