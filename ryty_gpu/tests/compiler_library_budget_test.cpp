#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "video_core/renderer_vulkan/shared_creation_cache.h"
#include "video_core/renderer_vulkan/compiler_worker_budget.h"
int main(){
 constexpr unsigned workers=Vulkan::CompilerWorkerBudget::ForLogicalThreads(16).pipeline;
 Vulkan::SharedCreationCache<unsigned,unsigned> cache;
 std::atomic<unsigned> active=0,peak=0,created=0,completed=0;
 std::mutex held_lock;std::vector<std::shared_ptr<unsigned>> held;
 std::barrier start{int(workers)};std::vector<std::thread> pool;
 for(unsigned w=0;w<workers;++w)pool.emplace_back([&,w]{
  auto tid=std::this_thread::get_id();start.arrive_and_wait();
  for(unsigned job=0;job<256;++job) for(unsigned stage=0;stage<2;++stage){
   unsigned key=stage*64+(job+w)%64;
   auto value=cache.GetOrCreate(key,[&,key]{
    assert(tid==std::this_thread::get_id());auto running=++active;auto old=peak.load();while(old<running&&!peak.compare_exchange_weak(old,running)){}
    std::this_thread::sleep_for(std::chrono::microseconds{20});++created;--active;return std::make_shared<unsigned>(key);
   });
   assert(*value==key);{std::scoped_lock lock{held_lock};held.push_back(value);}++completed;
  }
 });
 for(auto& t:pool)t.join();
 assert(created==128);assert(completed==3072);assert(peak<=workers);assert(active==0);
 held.clear();assert(!cache.Find(0));
 bool failed=false;try{cache.GetOrCreate(1000,[]{throw 7;return std::shared_ptr<unsigned>{};});}catch(int x){failed=x==7;}
 assert(failed);auto retried=cache.GetOrCreate(1000,[]{return std::make_shared<unsigned>(1);});assert(*retried==1);
 printf("PASS: 3072 stage acquisitions, 128 shared driver factories, peak %u <= %u workers; worker thread ownership, retirement and failed-build retry\n",peak.load(),workers);
}
