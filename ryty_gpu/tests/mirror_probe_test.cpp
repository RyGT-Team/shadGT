// SPDX-License-Identifier: GPL-2.0-or-later
// Production-extracted resume/probe branch, actual direct coverage and budget.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <thread>
#include "video_core/renderer_vulkan/optional_mirror_budget.h"
#include "video_core/renderer_vulkan/async_mirror_buffer.h"
using u64=std::uint64_t;using VAddr=u64;
template<class=void>struct ImportedPageCoverage{static constexpr u64 Limit=4096;};
namespace VideoCore {
struct Buffer{};
struct DirectMemory {
 struct View{u64 begin,end;std::shared_ptr<Buffer>wrapper;};
 std::map<u64,View>views;mutable std::mutex mutex;std::function<void(VAddr,u64)> warm;
 void WarmImported(VAddr a,u64 n){if(warm)warm(a,n);}
 enum class PartOutcome{Invalid,Busy,MissingCoverage,Covered};
 bool TryForEachPart(VAddr,u64,const std::function<bool()>&,const std::function<void(const Buffer&,u64,u64,u64)>&,PartOutcome* = nullptr)const;
};
}
using VideoCore::DirectMemory;using VideoCore::Buffer;
#include "direct-try-parts-under-test.inc"
struct Allocation{};
namespace Native::Rhi { using Buffer = Allocation; }
template<class T> using AsyncMirrorBuffer = Vulkan::AsyncMirrorBuffer<T>;
using Worker=Vulkan::AsyncMirrorBuffer<Allocation>;
using Clock=std::chrono::steady_clock;
static Clock::time_point clock_value{};
static Clock::duration step{};
static Clock::time_point FakeNow(){auto v=clock_value;clock_value+=step;return v;}
enum{MirrorSourceNotCalled,MirrorPromotionBudget};
struct Diagnostic{std::array<u64,3>source_outcomes{};u64 warm_attempts{},warm_queued{};};
struct PrepFixture {
 DirectMemory direct;std::atomic<u64> unmap_generation{};std::atomic<unsigned> allocations{};
 struct Cache {DirectMemory* direct;DirectMemory* GetDirectMemory(){return direct;}};
 struct Renderer {Cache buffer_cache;};Renderer r{{&direct}};
 std::unique_ptr<Allocation> Prepare(Worker::Request request){
#include "mirror-prepare-under-test.inc"
  ++allocations;return std::make_unique<Allocation>();
 }
};
struct Harness {
 DirectMemory storage; DirectMemory* direct=&storage;
 std::unique_ptr<Worker> worker=std::make_unique<Worker>([](auto){return std::make_unique<Allocation>();});
 Worker* mirror_preparation=worker.get();Vulkan::OptionalMirrorBudget mirror_promotion_budget;
 VAddr mirror_promotion_resume{};std::vector<std::pair<VAddr,VAddr>> pending_mirrors;
 std::atomic<u64> unmap_generation{};Diagnostic mirror_diagnostic;bool mirror_diag=false;
 unsigned copies{};bool unmap_after_probe{},use_worker{};VAddr attempted{};Worker::Observation last_observation=Worker::Observation::None;
 void NoteMirrorDiagnostic(int,u64,u64,u64,u64,u64){}
 void Visit(u64 frame){
  mirror_promotion_budget.BeginFrame(frame);
  if(pending_mirrors.empty()||!mirror_promotion_budget.Available())return;
  auto pending=std::move(pending_mirrors);pending_mirrors.clear();
#include "mirror-merge-under-test.inc"
#include "mirror-resume-under-test.inc"
#include "mirror-priority-under-test.inc"
  for(size_t range_index=0;range_index<ranges.size();++range_index){
   auto [begin,end]=ranges[range_index];
#include "mirror-probe-under-test.inc"
   if(unmap_after_probe)storage.views.clear();
   attempted=begin;bool source_callback=false;MirrorWorker::Observation observation=MirrorWorker::Observation::None;
   std::unique_ptr<Allocation> allocation;
   bool ready=direct->TryForEachPart(begin,end-begin,[&]{source_callback=true;if(!use_worker)return true;allocation=mirror_preparation->TryAcquire({begin,end,unmap_generation.load()},&observation);return bool(allocation);},[&](const Buffer&,u64,u64,u64){++copies;});
   last_observation=observation;
#include "mirror-requeue-under-test.inc"
  }
 }
};
static auto Order(std::vector<std::pair<VAddr,VAddr>> pending,VAddr mirror_promotion_resume) {
#include "mirror-merge-under-test.inc"
#include "mirror-resume-under-test.inc"
 return ranges;
}
int main(){
 assert((Order({{30,40},{0,10},{8,22},{50,60}},15)==std::vector<std::pair<VAddr,VAddr>>{{0,22},{30,40},{50,60}}));
 assert((Order({{30,40},{0,10},{8,22},{50,60}},25)==std::vector<std::pair<VAddr,VAddr>>{{30,40},{50,60},{0,22}}));
 // Legacy ordering charges before checking coverage: four missing ranges
 // consume all four starts, so a sixth fully covered range is never reached.
 Vulkan::OptionalMirrorBudget legacy;legacy.BeginFrame(1);unsigned reached=0;
 for(unsigned i=0;i<6;++i){if(!legacy.TryStart(4))break;++reached;}
 assert(reached==4 && reached<6);
 Vulkan::OptionalMirrorBudget limits;limits.BeginFrame(1);
 for(int i=0;i<4;++i)assert(limits.TryStart(1));assert(!limits.TryStart(1));
 limits.BeginFrame(2);assert(limits.TryStart(9ull<<20));assert(!limits.TryStart(1));
 limits.BeginFrame(3);assert(limits.TryStart(7ull<<20));assert(!limits.TryStart(2ull<<20));
 Harness h;for(u64 i=0;i<6;++i)h.pending_mirrors.emplace_back(i*10,i*10+4);
 h.storage.views.emplace(50,DirectMemory::View{50,54,std::make_shared<Buffer>()});
 // Hold the one worker slot occupied. Each probe and attempted warm accounts
 // 600 us, forcing an early cutoff without scheduler-dependent queue races.
 std::mutex block_mutex;std::condition_variable cv;bool started=false,release=false;
 h.worker=std::make_unique<Worker>([&](auto){std::unique_lock l(block_mutex);started=true;cv.notify_all();cv.wait(l,[&]{return release;});return std::make_unique<Allocation>();});h.mirror_preparation=h.worker.get();
 assert(h.worker->TryPrepare({500,504,0},[]{return true;}));
 {std::unique_lock l(block_mutex);cv.wait(l,[&]{return started;});}
 step=std::chrono::microseconds(600);for(u64 frame=1;frame<=6;++frame)h.Visit(frame);assert(h.copies==1);
 {std::scoped_lock l(block_mutex);release=true;}cv.notify_all();h.worker.reset();h.mirror_preparation=nullptr;
 Harness busy;busy.pending_mirrors={{0,4},{10,14}};
 {std::unique_lock lock{busy.storage.mutex};std::thread t([&]{step={};busy.Visit(1);});t.join();}
 assert(busy.pending_mirrors.size()==2 && busy.copies==0 && busy.mirror_promotion_resume==0);
 // Extracted worker guard refuses incomplete warming, retries to full coverage,
 // and never allocates on a changed generation or busy view mutex.
 PrepFixture prep;unsigned warms=0;
 prep.r.buffer_cache.direct=nullptr;assert(!prep.Prepare({0,8,0}) && prep.allocations==0);prep.r.buffer_cache.direct=&prep.direct;
 prep.direct.warm=[&](VAddr,u64){std::scoped_lock l(prep.direct.mutex);++warms;
  prep.direct.views[0]={0,4,std::make_shared<Buffer>()};
  if(warms>1)prep.direct.views[4]={4,8,std::make_shared<Buffer>()};};
 assert(!prep.Prepare({0,8,0}) && prep.allocations==0);
 assert(prep.Prepare({0,8,0}) && prep.allocations==1);
 prep.direct.warm=[&](VAddr,u64){++prep.unmap_generation;};
 assert(!prep.Prepare({0,8,0}) && prep.allocations==1);
 prep.direct.warm={};
 {std::unique_lock lock{prep.direct.mutex};std::thread t([&]{assert(!prep.Prepare({0,8,1}));});t.join();}
 assert(prep.allocations==1);
 PrepFixture retry;std::atomic<unsigned> retry_warms{};
 retry.direct.warm=[&](VAddr,u64){std::scoped_lock l(retry.direct.mutex);auto n=++retry_warms;
  retry.direct.views[0]={0,4,std::make_shared<Buffer>()};
  if(n>1)retry.direct.views[4]={4,8,std::make_shared<Buffer>()};};
 Worker retry_worker([&](auto req){return retry.Prepare(req);});
 assert(retry_worker.TryPrepare({0,8,0},[]{return true;}));
 const auto deadline=Clock::now()+std::chrono::seconds(5);
 while(retry_warms.load()<1){assert(Clock::now()<deadline);std::this_thread::yield();}
 while(!retry_worker.TryPrepare({0,8,0},[]{return true;})){assert(Clock::now()<deadline);std::this_thread::yield();}
 std::unique_ptr<Allocation> retried;
 while(!(retried=retry_worker.TryAcquire({0,8,0}))){assert(Clock::now()<deadline);std::this_thread::yield();}
 assert(retry_warms>=2 && retry.allocations==1);
 // A is running in the single worker while differently sized B precedes it
 // by address. The actual snapshot orders A first and Pending retains suffix.
 Harness priority;std::mutex pm;std::condition_variable pcv;bool pstarted=false,prelease=false;
 priority.worker=std::make_unique<Worker>([&](auto){std::unique_lock l(pm);pstarted=true;pcv.notify_all();pcv.wait(l,[&]{return prelease;});return std::make_unique<Allocation>();});priority.mirror_preparation=priority.worker.get();priority.use_worker=true;
 assert(priority.worker->TryPrepare({50,54,0},[]{return true;}));
 {std::unique_lock l(pm);pcv.wait(l,[&]{return pstarted;});}
 priority.pending_mirrors={{0,8},{50,54},{80,92}};
 priority.storage.views={{0,{0,8,std::make_shared<Buffer>()}},{50,{50,54,std::make_shared<Buffer>()}},{80,{80,92,std::make_shared<Buffer>()}}};
 step={};priority.Visit(1);assert(priority.attempted==50 && priority.copies==0 && priority.pending_mirrors.size()==3);
 {std::scoped_lock l(pm);prelease=true;}pcv.notify_all();
 const auto pd=Clock::now()+std::chrono::seconds(5);
 for(u64 frame=2;priority.copies==0;++frame){assert(Clock::now()<pd);priority.Visit(frame);std::this_thread::yield();}
 assert(priority.copies==1);priority.worker.reset();priority.mirror_preparation=nullptr;
 // A merged range grows past the owner's allocation: it is selected first,
 // discarded by the existing size rule, then retried at its complete size.
 Harness grown;grown.use_worker=true;grown.pending_mirrors={{0,8},{50,58},{80,92}};
 grown.storage.views={{0,{0,8,std::make_shared<Buffer>()}},{50,{50,58,std::make_shared<Buffer>()}},{80,{80,92,std::make_shared<Buffer>()}}};
 assert(grown.worker->TryPrepare({50,54,0},[]{return true;}));
 bool saw_discard=false;const auto gd=Clock::now()+std::chrono::seconds(5);
 for(u64 frame=1;grown.copies==0;++frame){assert(Clock::now()<gd);grown.Visit(frame);saw_discard |= grown.last_observation==Worker::Observation::DiscardSize;std::this_thread::yield();}
 assert(saw_discard && grown.copies==1);
 Harness changed;changed.pending_mirrors={{0,4},{10,14}};changed.storage.views.emplace(0,DirectMemory::View{0,4,std::make_shared<Buffer>()});changed.storage.views.emplace(10,DirectMemory::View{10,14,std::make_shared<Buffer>()});changed.unmap_after_probe=true;step={};changed.Visit(1);assert(changed.copies==0&&changed.attempted==0&&changed.pending_mirrors.size()==2);
}
