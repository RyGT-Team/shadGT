#include "ryty_gpu/native/cpu_stack_registry.h"
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <utility>
#include <thread>
using u64=uint64_t; using VAddr=u64;
namespace Common {template<class T>T AlignDown(T a,T n){return a&~(n-1);}template<class T>T AlignUp(T a,T n){return (a+n-1)&~(n-1);}}
struct Queue : std::vector<std::pair<u64,u64>> { bool fail{}; template<class... T> void emplace_back(T&&... args) { if(fail) throw 1; std::vector<std::pair<u64,u64>>::emplace_back(std::forward<T>(args)...); } };
struct NativeRenderer {
 std::mutex region_dirty_mutex;std::atomic<bool>has_pending_region_writes{};
 std::vector<std::pair<u64,u64>> pending_region_writes,dropped; Queue pending_stack_retirements; bool fail_write{};
 unsigned writes{};bool MirrorDiagnosticEnabled(){return false;}
 void OnCpuStack(u64,u64);void OnCpuWrite(u64 a,u64 n,u64){++writes;if(fail_write) throw 1;std::lock_guard l{region_dirty_mutex};pending_region_writes.emplace_back(a,a+n);has_pending_region_writes=true;}
 void DropMirrors(u64 a,u64 n){dropped.emplace_back(a,a+n);}
 void SortRegionWrites(){
#include "cpu-stack-sort.inc"
 }
};
#include "cpu-stack-on.inc"
struct Buffer{std::shared_ptr<int> owner;};
struct Stream{std::vector<std::unique_ptr<Buffer>>keep;bool buffers_written{};};
static bool Attempt(Stream& s,std::shared_mutex& buffer_map_mutex,u64 begin,u64 end,std::unique_ptr<Buffer> buffer){
 for(int once=0;once<1;++once){
#include "cpu-stack-copy.inc"
 return true;
 }
 return false;
}
int main(){
 NativeRenderer queue_fail;queue_fail.pending_stack_retirements.fail=true;
 assert(!Native::CpuStacks().Register(0x7000,4096,[&]{queue_fail.OnCpuStack(0x7000,4096);}));
 assert(queue_fail.writes==0 && !queue_fail.has_pending_region_writes.load());
 NativeRenderer write_fail;write_fail.fail_write=true;
 assert(!Native::CpuStacks().Register(0x8000,4096,[&]{write_fail.OnCpuStack(0x8000,4096);}));
 assert(write_fail.writes==1 && write_fail.has_pending_region_writes.load());
 write_fail.SortRegionWrites();assert(write_fail.dropped.size()==1);
 NativeRenderer n;assert(Native::CpuStacks().Register(0x1001,16,[&]{n.OnCpuStack(0x1001,16);}));
 assert(n.writes==1);n.SortRegionWrites();assert(n.dropped.size()==1);assert((n.dropped[0]==std::pair<u64,u64>{0x1000,0x2000}));
 std::shared_mutex map;Stream stream;auto owner=std::make_shared<int>(9);std::weak_ptr<int> weak=owner;
 assert(!Attempt(stream,map,0x1000,0x2000,std::make_unique<Buffer>(owner)));owner.reset();assert(!weak.expired());assert(stream.buffers_written&&stream.keep.size()==1);stream.keep.clear();assert(weak.expired());
 assert(Attempt(stream,map,0x2000,0x3000,std::make_unique<Buffer>()));Native::CpuStacks().Unregister(0x1001,16);
 std::atomic<bool> entered{}, release{};
 std::thread writer([&]{ assert(Native::CpuStacks().Register(0x4000,4096,[&]{entered=true;while(!release.load())std::this_thread::yield();})); });
 while(!entered.load()) std::this_thread::yield();
 Stream busy;auto retained=std::make_shared<int>(1);std::weak_ptr<int> busy_weak=retained;
 assert(!Attempt(busy,map,0x9000,0xa000,std::make_unique<Buffer>(retained)));retained.reset();
 assert(busy.buffers_written && busy.keep.size()==1 && !busy_weak.expired());
 release=true;writer.join();Native::CpuStacks().Unregister(0x4000,4096);busy.keep.clear();assert(busy_weak.expired());
}
