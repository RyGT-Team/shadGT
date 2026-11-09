#include "ryty_gpu/native/rhi/completed_resource_worker.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <future>
#include <cstdlib>
#include <new>
#include <memory>
#include <vector>
#include <array>
#include <deque>
static thread_local bool watch_allocations=false;
static thread_local size_t queue_allocations=0;
void* operator new(size_t n){if(watch_allocations)++queue_allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc{};}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete(void* p,size_t) noexcept{std::free(p);}
using u64=uint64_t;
using Native::Rhi::CompletedResourceWorker;
#define LOG_WARNING(...) ((void)0)
struct VulkanDevice {
 struct Item{u64 tick;std::function<void()> destroy;};
 std::mutex retire_mutex;std::deque<Item> retired;CompletedResourceWorker completed_resources;
 u64 completed=0;int queries=0;
 u64 CompletedTick(){++queries;return completed;}
 void Collect();
};
void VulkanDevice::Collect() {
    {
        std::scoped_lock lock{retire_mutex};
        if (retired.empty()) {
            return;
        }
    }
    const auto start = std::chrono::steady_clock::now();
    const u64 completed = CompletedTick();
    size_t count = 0;
    {
        std::scoped_lock lock{retire_mutex};
        while (count < 64 && !retired.empty() && retired.front().tick <= completed) {
            if (!completed_resources.TryEnqueue(retired.front().destroy)) {
                break; // Bounded backlog: retain ownership and retry at the next splice.
            }
            retired.pop_front();
            ++count;
        }
    }
    const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    if (elapsed >= 4.0) {
        LOG_WARNING(Render_Vulkan,
                    "NAT: slow resource handoff {:.2f} ms; {} completed objects queued, no inline destruction",
                    elapsed, count);
    }
}

int main(){
 const auto recorder=std::this_thread::get_id();
 {
  CompletedResourceWorker w;
  std::promise<void> entered,release;auto gate=release.get_future().share();
  std::function<void()> block=[&]{entered.set_value();gate.wait();};assert(w.TryEnqueue(block));entered.get_future().wait();
  std::atomic<int> done=0;
  for(int i=0;i<256;++i){std::function<void()> cb=[&,i]{assert(std::this_thread::get_id()!=recorder);assert(done.fetch_add(1)==i);};watch_allocations=true;assert(w.TryEnqueue(cb));watch_allocations=false;assert(!cb);}
  std::function<void()> full=[]{};assert(!w.TryEnqueue(full));assert(full);
  assert(queue_allocations==0);release.set_value();w.DrainAndStop();assert(done==256);assert(!w.TryEnqueue(full));w.DrainAndStop();
 }
 for(int run=0;run<32;++run){
  VulkanDevice d;std::atomic<int> done=0;d.completed=9;
  for(int i=0;i<1000;++i)d.retired.push_back({10,[&,i]{assert(std::this_thread::get_id()!=recorder);assert(done.fetch_add(1)==i);}});
  d.Collect();assert(done==0);assert(d.retired.size()==1000);
  d.completed=10;
  while(!d.retired.empty()) {auto before=d.retired.size();d.Collect();assert(before-d.retired.size()<=64);std::this_thread::yield();}
  d.completed_resources.DrainAndStop();assert(done==1000);
  auto queries=d.queries;d.Collect();assert(d.queries==queries);
 }
 {
  std::atomic<int> initialized=0;
  CompletedResourceWorker w{[&]{assert(std::this_thread::get_id()!=recorder);++initialized;}};
  std::array<std::atomic<int>,4> done{};std::vector<std::thread> producers;
  for(int p=0;p<4;++p)producers.emplace_back([&,p]{for(int i=0;i<2048;++i){std::function<void()> cb=[&,p,i]{assert(done[p].fetch_add(1)==i);assert(std::this_thread::get_id()!=recorder);};while(!w.TryEnqueue(cb))std::this_thread::yield();}});
  for(auto& t:producers)t.join();w.DrainAndStop();assert(initialized==1);for(auto& n:done)assert(n==2048);
 }
 for(int run=0;run<256;++run){
  CompletedResourceWorker w;std::atomic<int> accepted=0,done=0;std::promise<void> active;
  std::thread p{[&]{active.set_value();for(int i=0;i<128;++i){std::function<void()> cb=[&]{++done;};if(w.TryEnqueue(cb))++accepted;else assert(cb);}}};
  active.get_future().wait();w.DrainAndStop();p.join();assert(done==accepted);
 }
 {
  struct Payload {std::thread::id recorder;std::atomic<int>* done;~Payload(){assert(std::this_thread::get_id()!=recorder);++*done;}};
  std::atomic<int> destroyed=0;CompletedResourceWorker w;
  auto payload=std::make_shared<Payload>();payload->recorder=recorder;payload->done=&destroyed;
  std::function<void()> cb=[payload=std::move(payload)]{};
  while(!w.TryEnqueue(cb))std::this_thread::yield();w.DrainAndStop();assert(destroyed==1);
 }
 std::cout<<"PASS: zero queue allocations; 8192 four-producer FIFO callbacks; 256 concurrent producer/shutdown races; callback capture destroyed on worker; worker initialization hook\n";
 std::cout<<"PASS: 32256 worker callbacks; blocked-driver queue bound, ownership on rejection, FIFO, GPU completion, no recorder execution, 64 handoff cap, shutdown drain, empty-query bypass\n";
}
