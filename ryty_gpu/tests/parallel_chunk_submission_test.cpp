// SPDX-License-Identifier: GPL-2.0-or-later
// CPU ordering/fallback contract only. No GPU or fence retirement is simulated.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>
using u32=std::uint32_t;using u64=std::uint64_t;
#define NATIVE_DIAGNOSTIC(...) __VA_ARGS__
#define ASSERT_MSG(condition,...) assert(condition)
static std::vector<std::string> events;
static bool test_small_batch=false;
bool ParallelSmallPassBatchEnabled(){return test_small_batch;}
bool ParallelGateDiagEnabled(){return false;}
bool DescriptorThreadCpu100ns(u64&){return false;}
struct RecordPartitionClock {explicit RecordPartitionClock(int){}};
static int t_record_partition_end;
using VkCommandBuffer=void*;
namespace vk {
struct CommandBuffer {
    std::uintptr_t id{};
    CommandBuffer()=default;
    explicit CommandBuffer(VkCommandBuffer handle):id(reinterpret_cast<std::uintptr_t>(handle)){}
    explicit operator bool()const{return id!=0;}
    void endRendering(){events.push_back("end-rendering");}
};
}
struct RenderState {int id=1;bool operator==(const RenderState&)const=default;};
namespace Native::Rhi {
enum class CommandListLevel {Secondary};
struct CommandList {
    std::uintptr_t id=2;
    void Begin(){events.push_back("begin-replacement");}
    void End(){events.push_back("end-prefix");}
    std::uintptr_t NativeHandle()const{return id;}
};
struct Device {
    CommandList replacement{2};
    CommandList* AcquireCommandList(CommandListLevel){return &replacement;}
    void ReleaseUnsubmitted(CommandList*){events.push_back("release-replacement");}
};
}
namespace Native::Parallel {
struct Draw {std::size_t descriptor_bytes=16;};
struct Pass {RenderState target;std::vector<Draw> draws;};
template<int=0> struct SyncOwnerPins {static constexpr std::size_t Invalid=~std::size_t{0};};
struct RotationDiag {u64 attempts{},success{},invalid{},preflight{},acquire{},command{},prefix{},exception{};};
struct PassCoordinator {
    static constexpr std::size_t StreamDrawLimit=64;
    using StreamTicket=std::shared_ptr<Pass>;
    Native::Rhi::CommandList stream_replacement{2};
    void FinishStream(const StreamTicket& ticket) { assert(ticket && ticket->draws.size()==64); events.push_back("finish-stream"); }
    Native::Rhi::CommandList* StreamReplacement(const StreamTicket&) { events.push_back("replacement"); return &stream_replacement; }
    enum class TimingPhase {Rotation};
    struct TimingProbe {TimingProbe(PassCoordinator&,TimingPhase){} void Success(){}};
    bool allow_rotation=true,allow_prefix=true,allow_admission=true,throw_admission=false;
    int pass_calls=0,small_calls=0;
    bool PrefixPreflight(u64,Native::Rhi::CommandList*,const std::shared_ptr<const void>&,std::size_t){return allow_rotation;}
    bool Prefix(u64,Native::Rhi::CommandList* list,std::shared_ptr<const void>,std::size_t){
        assert(list->id==1);
        if(!allow_prefix){events.push_back("reject-prefix");return false;}
        events.push_back("prefix");return true;
    }
    bool TryPass(u64,const Pass& pass,std::size_t){
        ++pass_calls;assert(pass.draws.size()>=8);events.push_back("try-pass");
        if(throw_admission)throw std::bad_alloc();return allow_admission;
    }
    bool TryPassTransfer(u64 epoch,Pass& pass,std::size_t bytes){return TryPass(epoch,pass,bytes);}
    bool TrySmallPass(u64,const Pass&,std::size_t){++small_calls;events.push_back("try-small");return allow_admission;}
};
#include "chunk-rotate-segment.inc"
template<class Instance> bool RecordPass(vk::CommandBuffer cmd,const Instance&,const Pass& pass){
    assert(!pass.draws.empty());events.push_back("replay-"+std::to_string(cmd.id));return true;
}
}
#include "chunk-closure-types.inc"
struct PendingDescriptorUpdates {enum {EndPass};};
struct ParallelRuntime {
    struct Updates {void Flush(int,int){events.push_back("flush");}} descriptor_updates;
    struct Mode {bool Enabled()const{return false;}} transfer_applied_mode;
    struct Pins {
        void Queued(std::size_t slot){assert(slot==3);events.push_back("queued-pin");}
        bool Tag(std::size_t slot,u64 epoch){assert(slot==3 && epoch==7);events.push_back("tag-pin");return true;}
    } sync_pins;
    Native::Parallel::PassCoordinator actual_coordinator;
    Native::Parallel::PassCoordinator* coordinator=&actual_coordinator;
    std::shared_ptr<Native::Parallel::Pass> active;
    Native::Parallel::PassCoordinator::StreamTicket draw_stream;
    bool stream_attempted{};
    std::shared_ptr<const void> lease=std::make_shared<int>(42);
    std::vector<Native::Parallel::Draw> scratch_draws;
    bool closing=false;
    u64 epoch=7;std::size_t pin_slot=3;
    Native::Parallel::RotationDiag rotation_diag;
    PassClosureStats pass_closure;
    std::array<u64,9> pass_size_hist{};
    u64 small_admission_exceptions{},large_admission_exceptions{},queued{},synchronous{},coordinator_cpu_100ns{},coordinator_cpu_samples{};
    void FinishSerialEligibleRun(){}
};
struct NativeRenderer {
    struct Cache {int invalidations=0;void Invalidate(){++invalidations;}};
    struct Stream {
        bool rendering=true;RenderState render;Native::Rhi::CommandList old{1};
        Native::Rhi::CommandList* list=&old;vk::CommandBuffer cmd{reinterpret_cast<VkCommandBuffer>(1)};
        Cache dynamic,pipelines;bool vertex_state_known=true;
    } stream;
    struct {struct {int GetDevice(){return 0;}} instance;} r;
    Native::Rhi::Device actual_device;Native::Rhi::Device* device=&actual_device;
    ParallelRuntime runtime;
    explicit NativeRenderer(std::size_t draws){runtime.active=std::make_shared<Native::Parallel::Pass>();runtime.active->draws.resize(draws);}
    void EndRendering(u32 reason=0);
    bool ContinueSmallParallelPass(const RenderState&,u32,bool){return false;}
    void PassBarrier(){EndRendering();events.push_back("barrier");}
    void BeforeNextDraw(const RenderState& target,bool eligible=true,bool inline_safe=false){
        auto* parallel=&runtime;auto& s=stream;struct {RenderState state;} targets{target};
#include "chunk-close-before-draw.inc"
        events.push_back("next-draw");
    }
};
ParallelRuntime* FindParallelRuntime(NativeRenderer* renderer){return &renderer->runtime;}
#include "chunk-end-rendering.inc"

static std::size_t At(const std::string& value){auto it=std::find(events.begin(),events.end(),value);assert(it!=events.end());return it-events.begin();}
static int Count(const std::string& value){return static_cast<int>(std::count(events.begin(),events.end(),value));}
static void Clean(const NativeRenderer& r){
    assert(!r.runtime.active && !r.runtime.lease && !r.stream.rendering);
    assert(r.runtime.pin_slot==Native::Parallel::SyncOwnerPins<>::Invalid);
    assert(r.runtime.scratch_draws.empty() && r.runtime.scratch_draws.capacity()>=64);
    assert(r.stream.dynamic.invalidations>0 && r.stream.pipelines.invalidations>0 && !r.stream.vertex_state_known);
}
int main(){
    {events.clear();NativeRenderer r(63);r.BeforeNextDraw({});
     assert(r.runtime.active && r.runtime.active->draws.size()==63 && events==std::vector<std::string>{"next-draw"});}
    {events.clear();NativeRenderer r(64);r.BeforeNextDraw({});Clean(r);
     assert(At("prefix")<At("try-pass") && At("try-pass")<At("queued-pin") && At("queued-pin")<At("barrier") && At("barrier")<At("next-draw"));
     assert(!Count("tag-pin") && !Count("replay-1") && !Count("replay-2"));
     assert(r.runtime.queued==1 && r.runtime.synchronous==0 && r.runtime.pass_closure.reasons[2][5]==1);}
    {events.clear();NativeRenderer r(64);r.runtime.actual_coordinator.allow_rotation=false;r.BeforeNextDraw({});Clean(r);
     assert(r.stream.list==&r.stream.old && r.stream.cmd.id==1);
     assert(Count("replay-1")==1 && Count("tag-pin")==1 && !Count("prefix") && !Count("try-pass") && !Count("queued-pin"));}
    {events.clear();NativeRenderer r(64);r.runtime.actual_coordinator.allow_prefix=false;r.BeforeNextDraw({});Clean(r);
     assert(r.stream.list==&r.stream.old && r.stream.cmd.id==1);
     assert(At("reject-prefix")<At("release-replacement") && At("release-replacement")<At("replay-1"));
     assert(Count("replay-1")==1 && Count("tag-pin")==1 && !Count("end-prefix") && !Count("try-pass") && !Count("queued-pin"));}
    {events.clear();NativeRenderer r(64);r.runtime.actual_coordinator.allow_admission=false;r.BeforeNextDraw({});Clean(r);
     assert(r.stream.list==&r.actual_device.replacement && r.stream.cmd.id==2);
     assert(At("prefix")<At("try-pass") && At("try-pass")<At("replay-2") && At("replay-2")<At("tag-pin"));
     assert(Count("replay-2")==1 && !Count("replay-1") && !Count("queued-pin"));}
    {events.clear();NativeRenderer r(64);r.runtime.actual_coordinator.throw_admission=true;r.BeforeNextDraw({});Clean(r);
     assert(Count("replay-2")==1 && Count("tag-pin")==1 && r.runtime.large_admission_exceptions==1);}
    {events.clear();NativeRenderer r(7);test_small_batch=false;r.EndRendering(CloseTarget);
     assert(Count("replay-1")==1 && Count("tag-pin")==1 && !Count("prefix") && !Count("try-small"));
     assert(!r.runtime.active && !r.runtime.lease && r.runtime.synchronous==1);}
    {events.clear();NativeRenderer r(64);auto retained=r.runtime.active;r.runtime.draw_stream=retained;r.EndRendering(CloseFull);
     assert(At("finish-stream")<At("replacement") && At("replacement")<At("queued-pin"));
     assert(r.stream.list==&r.runtime.actual_coordinator.stream_replacement && r.stream.cmd.id==2);
     assert(retained->draws.size()==64 && !r.runtime.active && !r.runtime.lease);
     assert(!Count("prefix") && !Count("try-pass") && !Count("try-small") && !Count("replay-1") && !Count("replay-2") && !Count("tag-pin"));}
}
