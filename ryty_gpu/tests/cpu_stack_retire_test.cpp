#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>
#include "video_core/renderer_vulkan/page_watch_tracker.h"
#include "ryty_gpu/native/cpu_stack_registry.h"
namespace Common {template<class T>T AlignDown(T a,T n){return a&~(n-1);}template<class T>T AlignUp(T a,T n){return (a+n-1)&~(n-1);}}
using u64=uint64_t; using u32=uint32_t; using VAddr=u64;
namespace Native { template<int N> struct SlowCallTrace { SlowCallTrace(bool,const char*,std::initializer_list<const char*>) {} void Next(int, u64=0,u64=0) {} }; struct ImageEntry { struct {u64 guest_address,guest_size;} info;}; }
namespace VideoCore { enum class PageOp{Untrack}; struct PageManager { void UpdatePageWatchers(u64,u64,PageOp){}static u64 GetPageAddr(u64 a){return a&~4095ull;} static u64 GetNextPageAddr(u64 a){return (a+4096)&~4095ull;}}; }
struct Registry {template<class F>void ForEachOverlapping(u64,u64,F) {}};
struct Buffer {std::shared_ptr<int> lifetime;};
struct Range {u64 begin,end;Buffer buffer;};
struct Log {std::vector<std::pair<u64,u64>> writes;void Note(u64 a,u64 b){writes.emplace_back(a,b);}};
struct NativeRenderer {
 std::recursive_mutex mutex;std::shared_mutex buffer_map_mutex;std::map<u64,Range>mirrors,regions;std::vector<std::pair<u64,u64>> retired_watch_ranges,watches;Vulkan::PageWatchTracker page_watches; struct {VideoCore::PageManager page_manager;} r; std::shared_ptr<Registry>registry=std::make_shared<Registry>();std::atomic<u64>unmap_generation{},has_pending_region_writes{};std::mutex region_dirty_mutex;std::vector<std::pair<u64,u64>>pending_region_writes,pending_stack_retirements;u64 mirror_bytes{},buffer_generation{};Log buffer_log;
 bool MirrorDiagnosticEnabled(){return false;} template<class...T>void NoteMirrorDiagnostic(T...){};
 auto FirstMirror(u64 a){auto it=mirrors.upper_bound(a);if(it!=mirrors.begin()&&std::prev(it)->second.end>a)--it;return it;}
 void UnwatchPages(u64 b,u64 e);
 void DropMirrors(u64,u64,u32=~u32{0});void ReleaseUnusedWatches();void OnCpuStack(u64,u64);
 void OnCpuWrite(u64 a,u64 n,u64){page_watches.Untrack(a&~4095ull,(a+n+4095)&~4095ull,[](u64,u64){});std::lock_guard lock{region_dirty_mutex};pending_region_writes.emplace_back(a,a+n);has_pending_region_writes=true;}
 void SortRegionWrites(){
#include "cpu-stack-sort.inc"
 }
 // Fixture-only initial mirror and watch setup; retirement itself is extracted.
 void SeedMirror(u64 b,u64 e,std::shared_ptr<int> life){mirrors.emplace(b,Range{b,e,{life}});mirror_bytes+=e-b;page_watches.Track(b,e,[](u64,u64){});}

};
#include "cpu-stack-on.inc"
#include "cpu-stack-unwatch.inc"
#include "cpu-stack-drop.inc"
#include "cpu-stack-release.inc"
int main() {
 constexpr u64 b=0xf402004000,e=0xf402044000; NativeRenderer n;
 auto gpu=std::make_shared<int>(7);std::weak_ptr<int> weak=gpu;
 n.SeedMirror(0xf401fc4000,0xf402027000,gpu);auto pinned=gpu;gpu.reset();u64 proof=n.buffer_generation;
 assert(Native::CpuStacks().Register(b,e-b,[&]{n.OnCpuStack(b,e-b);}));
 n.SortRegionWrites();n.ReleaseUnusedWatches();assert(n.mirrors.empty());
 assert(n.buffer_generation!=proof && n.buffer_log.writes.size()==1);assert(!weak.expired());
 {Native::CpuStackRegistry::Lease l{Native::CpuStacks()};assert(!l.Allowed(b-4096,e+4096));assert(l.Allowed(e+4096,e+8192));}
 Native::CpuStacks().Unregister(b,e-b);pinned.reset();assert(weak.expired());
}
