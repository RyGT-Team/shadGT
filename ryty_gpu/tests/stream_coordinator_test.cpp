// Extracted production coordinator transaction, real worker queue and budget.
// Mock RHI records CPU ownership only; this is not a GPU/fence conformance test.
#define NDEBUG
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>
#include "draw_publication.h"
#include "ryty_gpu/native/ordered_pass_workers.h"
#include "ryty_gpu/native/pass_budget.h"
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
#define COORD_DIAG(...) do{}while(0)
#define COORD_RETURN(value) return (value)
using u64=std::uint64_t;
using VkCommandBuffer=void*;
namespace vk {struct CommandBuffer {std::uintptr_t id;explicit CommandBuffer(VkCommandBuffer handle):id(reinterpret_cast<std::uintptr_t>(handle)){}explicit operator bool()const{return id!=0;}};}
namespace Native::Rhi {
enum class CommandListLevel {Secondary};
struct CommandList {
    std::uintptr_t id{};std::atomic<bool> begun{},ended{};std::atomic<int> released{};
    explicit CommandList(std::uintptr_t value):id(value){}
    void Begin(){begun=true;}void End(){ended=true;}
    std::uintptr_t NativeHandle()const{return id;}
};
struct Device {
    std::mutex mutex;std::vector<std::unique_ptr<CommandList>> lists;
    std::atomic<bool> fail_acquire{};std::atomic<int> acquired{};
    CommandList* AcquireCommandList(CommandListLevel){
        ++acquired;if(fail_acquire.exchange(false))return nullptr;
        std::lock_guard lock(mutex);lists.push_back(std::make_unique<CommandList>(100+lists.size()));return lists.back().get();
    }
    void ReleaseUnsubmitted(CommandList* list){if(list)++list->released;}
};
struct Fence {bool Wait(u64,u64){return true;}u64 Completed(){return 7;}};
bool TryEnqueueCompleted(Device&,std::function<void()>& callback)noexcept{auto release=std::move(callback);release();return true;}
}
namespace Vulkan {
struct Instance {mutable std::atomic<std::size_t> emitted{};mutable std::atomic<int> active{},fallbacks{},failed{};mutable std::atomic<bool> fail_record{};};
}
namespace Native::Parallel {
struct Draw {int value{};std::size_t descriptor_bytes=16;std::shared_ptr<int> owner;};
struct Pass {int target=42;bool extended_verified=true;std::shared_ptr<const void> resource_lease;std::vector<Draw> draws;};
template<class T>struct CompletedDrawVectorPool {static constexpr std::size_t Invalid=~std::size_t{0};};
bool ValidatePass(const Pass& pass){return pass.resource_lease && !pass.draws.empty() &&
    std::all_of(pass.draws.begin(),pass.draws.end(),[](const Draw& draw){return draw.value>0 && bool(draw.owner);});}
bool RecordPass(vk::CommandBuffer,const Vulkan::Instance& instance,const Pass& pass){CHECK(ValidatePass(pass));++instance.fallbacks;instance.emitted+=pass.draws.size();return true;}
bool RecordPublishedPass(vk::CommandBuffer,const Vulkan::Instance& instance,const Pass&,const Draw* storage,DrawPublication& publication){
    ++instance.active;
    if(instance.fail_record.exchange(false)){++instance.failed;--instance.active;return false;}
    std::size_t consumed=0;
    for(;;){const auto ready=publication.Wait(consumed);for(;consumed<ready.count;++consumed){
        CHECK(storage[consumed].value==int(consumed+1) && storage[consumed].owner);
        ++instance.emitted;instance.emitted.notify_one();}
        if(ready.sealed)break;}
    --instance.active;return true;
}
class PassCoordinator {
public:
#include "stream-coordinator-types.inc"
private:
    Rhi::Device& device_;const Vulkan::Instance& instance_;Rhi::Fence& timeline_;
    PassBudget budget_;OrderedPassWorkers<GroupPtr,GroupPtr> workers_;
    GroupPtr pending_small_;std::deque<Node> nodes_;std::size_t node_cap_;
    std::vector<InFlight> in_flight_;std::size_t in_flight_count_{};bool failed_{};u64 pending_epoch_{};
    JobPtr Record(const JobPtr&,bool on_worker=false);
    std::function<void()> MakeRelease(const JobPtr& job){return [owner=job]{};}
    bool SubmitGroup(const GroupPtr&);void CloseSmall(unsigned)noexcept;
    bool ResolveGroup(const GroupPtr&,bool);static void DropGroupJob(const GroupPtr&,const JobPtr&)noexcept;
public:
    PassCoordinator(Rhi::Device& device,const Vulkan::Instance& instance,Rhi::Fence& fence,
        std::size_t queue_limit=32,std::size_t byte_limit=64*1024*1024,std::size_t node_limit=64)
        :device_(device),instance_(instance),timeline_(fence),budget_(node_limit,byte_limit),workers_(1,queue_limit),node_cap_(node_limit),in_flight_(node_limit){}
    ~PassCoordinator();
    StreamTicket BeginStream(u64,const std::shared_ptr<Pass>&,Rhi::CommandList*,std::shared_ptr<const void>);
    bool PublishStream(const StreamTicket&,std::size_t)noexcept;
    Rhi::CommandList* StreamReplacement(const StreamTicket&)const noexcept;
    void FinishStream(const StreamTicket&)noexcept;
    bool TryPass(u64,const Pass&,std::size_t);
    bool HasRoomForPass()const noexcept{const auto used=nodes_.size()+in_flight_count_;return used<=node_cap_ && node_cap_-used>=2;}
    std::size_t PendingNodes()const{return nodes_.size();}
    auto Budget(){return budget_.TakeSnapshot();}
#include "stream-drain-collect.inc"
};
#include "stream-coordinator-methods.inc"
}
using namespace Native::Parallel;
using namespace std::chrono_literals;
static std::shared_ptr<Pass> Source(std::size_t capacity=64,std::size_t initial=8){
    auto pass=std::make_shared<Pass>();pass->resource_lease=std::make_shared<int>(7);pass->draws.reserve(capacity);
    auto owner=std::make_shared<int>(42);for(std::size_t i=1;i<=initial;++i)pass->draws.push_back({int(i),16,owner});return pass;
}
static void WaitFor(const std::atomic<std::size_t>& value,std::size_t target){
    const auto deadline=std::chrono::steady_clock::now()+2s;
    while(value.load()<target){CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
}
static auto Accept(std::vector<Native::Rhi::CommandList*>& lists){
    return [&lists](Native::Rhi::CommandList* list,PassCoordinator::JobPtr,PassCoordinator::PrefixPtr)noexcept{lists.push_back(list);};
}
int main(){
    // Starting with fewer than eight draws still runs the worker while the
    // producer is open. Each subsequent slot is published individually.
    for(std::size_t initial:{1,2,7}){Native::Rhi::Device device;Vulkan::Instance instance;Native::Rhi::Fence fence;Native::Rhi::CommandList prefix{1};
     PassCoordinator coordinator(device,instance,fence);auto source=Source(64,initial);const Draw* storage=source->draws.data();
     std::weak_ptr<int> owner=source->draws[0].owner;
     auto ticket=coordinator.BeginStream(7,source,&prefix,source->resource_lease);CHECK(ticket && coordinator.PendingNodes()==2);
     WaitFor(instance.emitted,initial);std::vector<Native::Rhi::CommandList*> accepted;accepted.reserve(2);
     CHECK(!coordinator.Drain(7,Accept(accepted)) && accepted.empty() && !prefix.ended);
     for(std::size_t count=initial+1;count<=64;++count){
        source->draws.push_back({int(count),16,source->draws[0].owner});CHECK(source->draws.data()==storage);
        CHECK(coordinator.PublishStream(ticket,count));WaitFor(instance.emitted,count);
        CHECK(!coordinator.Drain(7,Accept(accepted)) && accepted.empty() && !prefix.ended);
     }
     coordinator.FinishStream(ticket);CHECK(prefix.ended && source->draws.empty() && ticket->pass.draws.data()==storage);
     auto* replacement=coordinator.StreamReplacement(ticket);source.reset();
     CHECK(coordinator.Drain(7,Accept(accepted)) && accepted.size()==2 && accepted[0]==&prefix && accepted[1]==ticket->list);ticket.reset();
     CHECK(instance.emitted==64 && instance.fallbacks==0 && !owner.expired());
     coordinator.Collect(6);CHECK(!owner.expired());coordinator.Collect(7);CHECK(owner.expired());device.ReleaseUnsubmitted(replacement);}
    // Open streams cannot consume futures or reach the primary. The worker has
    // nevertheless consumed immutable draw slots before FinishStream.
    {Native::Rhi::Device device;Vulkan::Instance instance;Native::Rhi::Fence fence;Native::Rhi::CommandList prefix{1};
     PassCoordinator coordinator(device,instance,fence);auto source=Source();const Draw* storage=source->draws.data();
     auto ticket=coordinator.BeginStream(7,source,&prefix,source->resource_lease);CHECK(ticket && coordinator.PendingNodes()==2);
     WaitFor(instance.emitted,8);std::vector<Native::Rhi::CommandList*> accepted;accepted.reserve(2);
     CHECK(!coordinator.Drain(7,Accept(accepted)) && accepted.empty() && !prefix.ended);
     source->draws.push_back({9,16,source->draws[0].owner});CHECK(coordinator.PublishStream(ticket,9));
     CHECK(!coordinator.PublishStream(ticket,8));coordinator.FinishStream(ticket);
     CHECK(prefix.ended && source->draws.empty() && ticket->pass.draws.data()==storage);
     CHECK(coordinator.Drain(7,Accept(accepted)) && accepted.size()==2 && accepted[0]==&prefix && accepted[1]==ticket->list);
     CHECK(instance.emitted==9 && instance.fallbacks==0);coordinator.Collect(7);
     device.ReleaseUnsubmitted(coordinator.StreamReplacement(ticket));}
    // Two accepted streams retain the prefix/pass pairs in source order.
    {Native::Rhi::Device device;Vulkan::Instance instance;Native::Rhi::Fence fence;Native::Rhi::CommandList a{1},b{2};
     PassCoordinator coordinator(device,instance,fence);auto first=Source(),second=Source();
     auto x=coordinator.BeginStream(7,first,&a,first->resource_lease);CHECK(x);coordinator.FinishStream(x);
     auto y=coordinator.BeginStream(7,second,&b,second->resource_lease);CHECK(y);coordinator.FinishStream(y);
     std::vector<Native::Rhi::CommandList*> accepted;accepted.reserve(4);CHECK(coordinator.Drain(7,Accept(accepted)));
     CHECK(accepted.size()==4 && accepted[0]==&a && accepted[1]==x->list && accepted[2]==&b && accepted[3]==y->list);
     coordinator.Collect(7);}
    // Existing bulk groups before and after a stream keep future/node order.
    {Native::Rhi::Device device;Vulkan::Instance instance;Native::Rhi::Fence fence;Native::Rhi::CommandList prefix{1};
     PassCoordinator coordinator(device,instance,fence);auto a=Source(),stream=Source(),b=Source();a->target=101;b->target=202;
     CHECK(coordinator.TryPass(7,*a,sizeof(Pass)+a->draws.capacity()*sizeof(Draw)));
     auto ticket=coordinator.BeginStream(7,stream,&prefix,stream->resource_lease);CHECK(ticket);coordinator.FinishStream(ticket);
     CHECK(coordinator.TryPass(7,*b,sizeof(Pass)+b->draws.capacity()*sizeof(Draw)));
     std::vector<int> targets;targets.reserve(4);
     CHECK(coordinator.Drain(7,[&](Native::Rhi::CommandList*,PassCoordinator::JobPtr job,PassCoordinator::PrefixPtr)noexcept{targets.push_back(job?job->pass.target:-1);}));
     CHECK((targets==std::vector<int>{101,-1,42,202}));coordinator.Collect(7);}
    // Queue rejection rolls back both provisional nodes, credits and replacement.
    {Native::Rhi::Device device;Vulkan::Instance instance;Native::Rhi::Fence fence;Native::Rhi::CommandList a{1},b{2};
     PassCoordinator coordinator(device,instance,fence,1);auto first=Source(),second=Source();
     auto x=coordinator.BeginStream(7,first,&a,first->resource_lease);CHECK(x);WaitFor(instance.emitted,8);const auto charged=coordinator.Budget().bytes;
     CHECK(!coordinator.BeginStream(7,second,&b,second->resource_lease));
     CHECK(coordinator.PendingNodes()==2 && coordinator.Budget().count==2 && coordinator.Budget().bytes==charged && second->draws.size()==8 && !b.ended);
     CHECK(std::count_if(device.lists.begin(),device.lists.end(),[](const auto& list){return list->released==1;})==1);
     coordinator.FinishStream(x);std::vector<Native::Rhi::CommandList*> accepted;accepted.reserve(2);CHECK(coordinator.Drain(7,Accept(accepted)));coordinator.Collect(7);}
    // Budget and replacement acquisition failures leave no accepted work.
    for(std::size_t initial:{1,2,7,8})for(bool fail_acquire:{false,true}){Native::Rhi::Device device;Vulkan::Instance instance;Native::Rhi::Fence fence;Native::Rhi::CommandList prefix{1};
     device.fail_acquire=fail_acquire;PassCoordinator coordinator(device,instance,fence,32,fail_acquire?64*1024*1024:1024);auto source=Source(64,initial);
     CHECK(!coordinator.BeginStream(7,source,&prefix,source->resource_lease));CHECK(coordinator.PendingNodes()==0 && coordinator.Budget().count==0 && source->draws.size()==initial && !prefix.ended && instance.emitted==0 && instance.fallbacks==0);}
    // Initial and incremental descriptor charges must stay within the reserved
    // per-draw cap; rejected unpublished tail slots cannot reach the worker.
    {Native::Rhi::Device device;Vulkan::Instance instance;Native::Rhi::Fence fence;Native::Rhi::CommandList prefix{1};
     PassCoordinator coordinator(device,instance,fence);auto source=Source();source->draws[0].descriptor_bytes=PassCoordinator::StreamDescriptorBytesPerDraw+1;
     CHECK(!coordinator.BeginStream(7,source,&prefix,source->resource_lease) && coordinator.PendingNodes()==0 && coordinator.Budget().count==0);
     source->draws[0].descriptor_bytes=16;auto ticket=coordinator.BeginStream(7,source,&prefix,source->resource_lease);CHECK(ticket);
     source->draws.push_back({9,PassCoordinator::StreamDescriptorBytesPerDraw+1,source->draws[0].owner});CHECK(!coordinator.PublishStream(ticket,9));
     CHECK(ticket->stream->publication.Load().count==8);source->draws.pop_back();coordinator.FinishStream(ticket);
     std::vector<Native::Rhi::CommandList*> accepted;accepted.reserve(2);CHECK(coordinator.Drain(7,Accept(accepted)) && instance.emitted==8);coordinator.Collect(7);}
    // Reused capacity 256 is charged in full, despite a 64-packet publication cap.
    {std::size_t bytes[2]{};
     for(int i=0;i<2;++i){Native::Rhi::Device device;Vulkan::Instance instance;Native::Rhi::Fence fence;Native::Rhi::CommandList prefix{1};
        PassCoordinator coordinator(device,instance,fence);auto source=Source(i?256:64);auto ticket=coordinator.BeginStream(7,source,&prefix,source->resource_lease);CHECK(ticket);
        bytes[i]=coordinator.Budget().bytes;coordinator.FinishStream(ticket);std::vector<Native::Rhi::CommandList*> accepted;accepted.reserve(2);CHECK(coordinator.Drain(7,Accept(accepted)));coordinator.Collect(7);}
     CHECK(bytes[1]-bytes[0]==192*sizeof(Draw));}
    // Failed worker recording is discarded and replayed once after sealing.
    for(std::size_t initial:{1,2,7,8}){Native::Rhi::Device device;Vulkan::Instance instance;Native::Rhi::Fence fence;Native::Rhi::CommandList prefix{1};
     instance.fail_record=true;PassCoordinator coordinator(device,instance,fence);auto source=Source(64,initial);auto ticket=coordinator.BeginStream(7,source,&prefix,source->resource_lease);CHECK(ticket);
     for(std::size_t count=initial+1;count<=64;++count){source->draws.push_back({int(count),16,source->draws[0].owner});CHECK(coordinator.PublishStream(ticket,count));}
     coordinator.FinishStream(ticket);std::vector<Native::Rhi::CommandList*> accepted;accepted.reserve(2);CHECK(coordinator.Drain(7,Accept(accepted)));
     CHECK(accepted.size()==2 && accepted[0]==&prefix && instance.failed==1 && instance.fallbacks==1 && instance.emitted==64);coordinator.Collect(7);}
    // Accepted storage and owners survive source/ticket release and GPU drain.
    {Native::Rhi::Device device;Vulkan::Instance instance;Native::Rhi::Fence fence;Native::Rhi::CommandList prefix{1};
     PassCoordinator coordinator(device,instance,fence);auto source=Source();std::weak_ptr<int> owner=source->draws[0].owner;
     auto ticket=coordinator.BeginStream(7,source,&prefix,source->resource_lease);CHECK(ticket);coordinator.FinishStream(ticket);source.reset();ticket.reset();
     std::vector<Native::Rhi::CommandList*> accepted;accepted.reserve(2);CHECK(coordinator.Drain(7,Accept(accepted)) && !owner.expired());
     coordinator.Collect(6);CHECK(!owner.expired());coordinator.Collect(7);CHECK(owner.expired() && coordinator.Budget().count==0);}
    // Abandoning an open producer wakes and joins its worker before releasing
    // source slots and unsubmitted prefix/replacement/recording command lists.
    {auto teardown=std::async(std::launch::async,[]{
        Native::Rhi::Device device;Vulkan::Instance instance;Native::Rhi::Fence fence;Native::Rhi::CommandList prefix{1};auto source=Source();std::weak_ptr<int> owner=source->draws[0].owner;
        {PassCoordinator coordinator(device,instance,fence);auto ticket=coordinator.BeginStream(7,source,&prefix,source->resource_lease);CHECK(ticket);WaitFor(instance.emitted,8);source.reset();ticket.reset();}
        CHECK(instance.active==0 && prefix.released==1 && owner.expired());
        for(const auto& list:device.lists)CHECK(list->released==1);
     });CHECK(teardown.wait_for(2s)==std::future_status::ready);teardown.get();}
}
