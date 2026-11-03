// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "video_core/renderer_vulkan/async_mirror_buffer.h"
using u64=std::uint64_t;using VAddr=u64;
template<class = void> struct ImportedPageCoverage {static constexpr u64 Limit=1024;};
struct Buffer {std::vector<unsigned char> bytes;};
struct DirectMemory {
 struct View {u64 begin,end;std::shared_ptr<Buffer> wrapper;};
 std::map<u64,View> views;mutable std::mutex mutex;
 bool TryForEachPart(VAddr,u64,const std::function<bool()>&,const std::function<void(const Buffer&,u64,u64,u64)>&) const;
};
#include "direct-try-parts-under-test.inc"
struct Shared {std::atomic<unsigned> made{},destroyed{};std::mutex mutex;std::thread::id destroyed_by;};
struct Allocation {explicit Allocation(Shared& s,u64 size):shared(s),bytes(size,0xee){} Shared& shared;std::vector<unsigned char> bytes;
 ~Allocation(){std::scoped_lock lock(shared.mutex);shared.destroyed_by=std::this_thread::get_id();++shared.destroyed;}};
using Worker=Vulkan::AsyncMirrorBuffer<Allocation>;using Obs=Worker::Observation;
template<class F> void Until(F fn){auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!fn()){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}}
int main(){
 Shared shared;Worker worker([&](auto r){++shared.made;return std::make_unique<Allocation>(shared,r.end-r.begin);});
 Obs obs=Obs::Reused;assert(!worker.TryAcquire({10,10,1},&obs) && obs==Obs::None);
 obs=Obs::Reused;assert(!worker.TryAcquire({~u64{0},0,1},&obs) && obs==Obs::None);
 obs=Obs::Reused;assert(!worker.TryAcquire({0,(256ull<<20)+1,1},&obs) && obs==Obs::None);
 assert(!worker.TryAcquire({~u64{0}-1,~u64{0},1},&obs) && obs==Obs::Queued);
 // Retire the queued one-byte request on a different-size request, on worker.
 Until([&]{worker.TryAcquire({30,38,1},&obs);return obs==Obs::DiscardSize;});
 Until([&]{return shared.destroyed.load()==1;});{std::scoped_lock lock(shared.mutex);assert(shared.destroyed_by!=std::this_thread::get_id());}
 // Request A, but validate and copy both views of B before considering reuse complete.
 Until([&]{worker.TryAcquire({10,18,2},&obs);return obs==Obs::Queued;});
 DirectMemory direct;auto b1=std::make_shared<Buffer>();b1->bytes={11,12,13,14};auto b2=std::make_shared<Buffer>();b2->bytes={15,16,17,18};
 direct.views={{100,{100,104,b1}}};std::unique_ptr<Allocation> allocation;unsigned ready_calls=0,copies=0;
 const auto ready=[&]{++ready_calls;allocation=worker.TryAcquire({100,108,2},&obs);return bool(allocation);};
 const auto copy=[&](const Buffer& src,u64 off,u64 dst,u64 count){++copies;std::copy_n(src.bytes.begin()+off,count,allocation->bytes.begin()+dst);};
 assert(!direct.TryForEachPart(100,8,ready,copy) && ready_calls==0 && copies==0);
 direct.views.emplace(104,DirectMemory::View{104,108,b2});
 {std::unique_lock lock(direct.mutex);std::thread t([&]{assert(!direct.TryForEachPart(100,8,ready,copy));});t.join();assert(ready_calls==0 && copies==0);}
 Until([&]{return direct.TryForEachPart(100,8,ready,copy);});
 assert(obs==Obs::Reused && shared.made.load()==2 && copies==2 && (allocation->bytes==std::vector<unsigned char>{11,12,13,14,15,16,17,18}));allocation.reset();
 // Same-address match succeeds; a generation mismatch is discarded on the worker.
 Until([&]{worker.TryAcquire({20,28,3},&obs);return obs==Obs::Queued;});
 Until([&]{worker.TryAcquire({50,58,4},&obs);return obs==Obs::DiscardGeneration;});
 Until([&]{return shared.destroyed.load()>=3;});{std::scoped_lock lock(shared.mutex);assert(shared.destroyed_by!=std::this_thread::get_id());}
 Until([&]{worker.TryAcquire({20,28,4},&obs);return obs==Obs::Queued;});
 std::unique_ptr<Allocation> same;Until([&]{same=worker.TryAcquire({20,28,4},&obs);return bool(same);});assert(obs==Obs::Ready);same.reset();
 // A failed allocation returns the slot to Idle and a later retry can succeed.
 std::atomic<unsigned> tries{};Worker retry([&](auto r)->std::unique_ptr<Allocation>{if(++tries==1)return {};return std::make_unique<Allocation>(shared,r.end-r.begin);});
 std::unique_ptr<Allocation> recovered;Until([&]{recovered=retry.TryAcquire({60,68,7},&obs);return bool(recovered);});assert(tries>=2);recovered.reset();
 // While worker allocation is blocked, requests do not wait; shutdown waits and
 // abandoned prepared storage is destroyed by that worker after it is released.
 Shared shutdown;std::mutex m;std::condition_variable cv;bool started=false,release=false;
 auto blocking=std::make_unique<Worker>([&](auto r){std::unique_lock l(m);started=true;cv.notify_all();cv.wait(l,[&]{return release;});return std::make_unique<Allocation>(shutdown,r.end-r.begin);});
 assert(!blocking->TryAcquire({1,9,9},&obs));{std::unique_lock l(m);cv.wait(l,[&]{return started;});}
 assert(!blocking->TryAcquire({3,11,9},&obs) && (obs==Obs::Pending || obs==Obs::Busy));
 std::thread::id closer_id;std::thread closer([&]{closer_id=std::this_thread::get_id();blocking.reset();});{std::scoped_lock l(m);release=true;}cv.notify_all();closer.join();
 assert(shutdown.destroyed==1);{std::scoped_lock lock(shutdown.mutex);assert(shutdown.destroyed_by!=std::this_thread::get_id() && shutdown.destroyed_by!=closer_id);}
}
