#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
using u32=std::uint32_t; using u64=std::uint64_t;
#define ASSERT_MSG(x,...) assert(x)
struct RenderState { int id=1; bool operator==(const RenderState&)const=default; };
namespace vk {using Pipeline=int;}
static bool race=true,dump=false,scene=false,checkpoint=false,valid=true,batch=false;
static int flushes=0,replays=0,tags=0,closes=0;
bool RytyGtsInRace(){return race;}
namespace SceneTrace {int Id(){return scene;}}
namespace Native { namespace GpuCheckpoints {bool Requested(){return checkpoint;}} }
struct Pass {RenderState target;std::vector<int> draws{1};};
struct PendingDescriptorUpdates {enum {EndPass};};
namespace Native::Parallel {
template<int=0> struct SyncOwnerPins {static constexpr int Invalid=-1;};
bool ValidatePass(const Pass&){return valid;}
template<class Instance> bool RecordPassInline(int, Instance&, const Pass&, int& dynamic, vk::Pipeline& pipeline){++replays;dynamic=42;pipeline=77;return true;}
}
bool ParallelSmallPassBatchEnabled(){return batch;}
bool ParallelGateDiagEnabled(){return false;}
bool DescriptorThreadCpu100ns(u64&){return false;}
struct ParallelRuntime {
 struct Updates {enum {EndPass}; void Flush(int,int){++flushes;}} descriptor_updates;
 struct Pins {static constexpr int Invalid=-1;bool Tag(int,u64){++tags;return true;}} sync_pins;
 struct Closure {void Close(std::size_t,bool,u32){++closes;}} pass_closure;
 std::optional<Pass> active{Pass{}};bool draw_stream=false,closing=false;int active_list=1,active_cmd=2,pin_slot=3;
 u64 epoch=4,inline_continuations=0,synchronous=0,coordinator_cpu_100ns=0,coordinator_cpu_samples=0;
 std::array<u64,9> pass_size_hist{};std::vector<int> scratch_draws;std::optional<int> lease=1;
 void FinishSerialEligibleRun(){}
};
struct NativeRenderer {
 struct Stream {bool rendering=true;RenderState render{};int list=1,cmd=2,dynamic=0;struct {std::array<int,2> handles{0,88};} pipelines;bool vertex_state_known=true;} stream;
 struct R {struct Instance {int GetDevice(){return 0;}} instance;struct Scheduler {u64 tick=4;u64 CurrentTick(){return tick;}} scheduler;bool FrameDumpActive(){return dump;}} r;
 ParallelRuntime runtime;bool ContinueSmallParallelPass(const RenderState&,u32,bool);
};
ParallelRuntime* FindParallelRuntime(NativeRenderer* r){return &r->runtime;}
struct Lease {void reset(){}};

#include "inline-continuation-under-test.inc"
void ResetFlags(){race=true;dump=scene=checkpoint=false;valid=true;batch=false;flushes=replays=tags=closes=0;}
void Reject(NativeRenderer& r,bool safe=true){
 assert(!r.ContinueSmallParallelPass(RenderState{},1,safe));
 assert(!flushes && !replays && !tags && !closes);
 assert(r.runtime.active && r.runtime.pin_slot==3 && r.runtime.lease && r.stream.rendering);
 assert(r.stream.dynamic==0 && r.stream.pipelines.handles[0]==0 && r.stream.vertex_state_known);
}
int main(){
 {ResetFlags();NativeRenderer r;race=false;Reject(r);}
 {ResetFlags();NativeRenderer r;dump=true;Reject(r);}
 {ResetFlags();NativeRenderer r;scene=true;Reject(r);}
 {ResetFlags();NativeRenderer r;checkpoint=true;Reject(r);}
 {ResetFlags();NativeRenderer r;valid=false;Reject(r);}
 {ResetFlags();NativeRenderer r;r.stream.list=7;Reject(r);}
 {ResetFlags();NativeRenderer r;r.stream.cmd=7;Reject(r);}
 {ResetFlags();NativeRenderer r;r.r.scheduler.tick=7;Reject(r);}
 {ResetFlags();NativeRenderer r;r.stream.render.id=7;Reject(r);}
 {ResetFlags();NativeRenderer r;r.runtime.active->target.id=7;Reject(r);}
 {ResetFlags();NativeRenderer r;r.runtime.active->draws.clear();Reject(r);}
 {ResetFlags();NativeRenderer r;r.runtime.active->draws.resize(8);Reject(r);}
 {ResetFlags();NativeRenderer r;batch=true;Reject(r);}
 {ResetFlags();NativeRenderer r;r.runtime.closing=true;Reject(r);}
 {ResetFlags();NativeRenderer r;r.runtime.draw_stream=true;Reject(r);}
 {ResetFlags();NativeRenderer r;r.stream.rendering=false;assert(!r.ContinueSmallParallelPass(RenderState{},1,true));assert(!flushes && !replays && !tags && r.runtime.active);}
 {ResetFlags();NativeRenderer r;Reject(r,false);}
 {ResetFlags();NativeRenderer r;assert(r.ContinueSmallParallelPass(RenderState{},1,true));
  assert(flushes==1 && replays==1 && tags==1 && closes==1 && !r.runtime.active);
  assert(r.runtime.pin_slot==-1 && !r.runtime.lease && r.runtime.synchronous==1 && r.runtime.inline_continuations==1);
  assert(r.runtime.pass_size_hist[1]==1 && r.stream.rendering && r.stream.dynamic==42);
  assert(r.stream.pipelines.handles[0]==77 && r.stream.pipelines.handles[1]==88 && !r.stream.vertex_state_known);
 }
}

