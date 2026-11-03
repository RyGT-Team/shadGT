#include <atomic>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <thread>
#include <vector>
#include "ryty_gpu/native/sync_owner_pins.h"
using u64=unsigned long long;
struct VertexIndexState { int value{}; std::vector<int> attributes,input_bindings,buffers,offsets,sizes,strides; std::shared_ptr<const void> pipeline_owner; };
struct Draw {std::shared_ptr<const VertexIndexState> vertex_index; std::shared_ptr<const void> pipeline_owner;};
struct Draws : std::vector<Draw> {
 std::size_t fake_capacity{};
 std::size_t capacity() const {return fake_capacity ? fake_capacity : std::vector<Draw>::capacity();}
};
struct Pass {Draws draws;};
constexpr std::size_t VertexLeaseCharge=sizeof(VertexIndexState)+128;
#include "vertex-lease-budget.inc"
std::atomic<u64> vertex_lease_fresh{},vertex_lease_replaced{},vertex_lease_borrowed{};
struct Prepared {std::shared_ptr<VertexIndexState> vertex_index; bool vertex_index_published{}; std::shared_ptr<const void> pipeline_keepalive;};
bool Eligible(Prepared* prepared,const VertexIndexState* vi,bool is_indexed,bool& leaseable_vi) {
 bool descriptor_safe=true;
#include "lease-eligibility.inc"
 return eligible;
}
bool fail_alloc=false;
std::shared_ptr<VertexIndexState> MakeVertex(){if(fail_alloc)throw std::bad_alloc{};return std::make_shared<VertexIndexState>();}
bool Prepare(Prepared& p) {
#include "vertex-lease-producer.inc"
 return true;
}
Draw Capture(Prepared* prepared,std::function<bool(const std::shared_ptr<const void>&)> test_pin=[](const auto&){return true;},bool fail_before_publication=false) {
 Draw captured;bool leaseable_vi=true;if(fail_before_publication)throw std::bad_alloc{};
#include "vertex-lease-capture.inc"
 return captured;
}
using u32=unsigned;
namespace vk {
using DescriptorSet=unsigned;using DescriptorBufferInfo=int;using DescriptorImageInfo=int;
enum class DescriptorType {eUniformBuffer,eStorageBuffer,eSampler,eSampledImage,eOther};
}
struct DescriptorSnapshot {
 static bool BufferSliceValid(const int* p,unsigned n,std::span<const int> src){return p && n<=src.size() && p>=src.data() && p+n<=src.data()+src.size();}
 static bool ImageSliceValid(const int* p,unsigned n,std::span<const int> src){return BufferSliceValid(p,n,src);}
};
struct Row {unsigned dstSet{},descriptorCount{};const int* pBufferInfo{};const int* pImageInfo{};const void* pNext{};const void* pTexelBufferView{};vk::DescriptorType descriptorType{};};
struct Batch {enum class Admission {Yes,Capacity,Unsupported};std::array<Row,8> writes{};std::array<int,16> buffers{},images{};std::size_t nw{},nb{},ni{};u64 staged_rows{},high_rows{};
#include "descriptor-stage.inc"
#include "descriptor-check.inc"
};
namespace validation {
using u32=unsigned;using u64=unsigned long long;
namespace vk {
using DeviceSize=unsigned long long;
enum class IndexType {eUint16,eUint32,eOther};
enum class DescriptorType {eSampler,eSampledImage,eUniformBuffer,eStorageBuffer,eOther};
}
namespace Shader::Backend {constexpr unsigned MaxDescriptorSets=4;}
struct Binding {unsigned binding{};const void* pNext{};};
struct VI {std::vector<Binding> attributes,input_bindings;std::vector<int> buffers,offsets,sizes,strides;};
struct Write {const void* pNext{};const void* pTexelBufferView{};vk::DescriptorType descriptorType{};const void* pBufferInfo{};const void* pImageInfo{};};
struct Snapshot {std::vector<Write> writes;const auto& Writes()const{return writes;}};
struct Draw {int pipeline=1,layout=1;std::shared_ptr<const void> pipeline_owner=std::make_shared<int>(1);std::array<int,4> sets{};unsigned push_set=4,required_sets_mask{};bool indexed{},index_backing_verified{},vertex_backing_verified{},immutable_upload_buffers_verified{};int index_buffer{};vk::IndexType index_type{};vk::DeviceSize index_offset{},index_size{};unsigned vertices{};std::shared_ptr<VI> vertex_index;unsigned vertex_attribute_count{},vertex_binding_count{},vertex_buffer_count{};std::shared_ptr<Snapshot> push_writes;};
using VertexIndexState=VI;
struct Attachment {int image_view=1;bool is_clear{};};
struct Depth {bool has_depth{},has_stencil{};int image_view{};};
struct Target {unsigned num_color_attachments=1,width=1,height=1,num_layers=1;std::array<Attachment,8> color_attachments;Depth depth_stencil_attachment;};
struct Pass {Target target;bool extended_verified{};std::shared_ptr<const void> resource_lease=std::make_shared<int>(1);std::vector<Draw> draws;};
#include "vertex-validate.inc"
}
struct DrawPool {static constexpr std::size_t Invalid=~std::size_t{0};};
struct Loan {std::size_t index=3;Draws spare;};
struct Job {std::size_t loan=DrawPool::Invalid;Pass pass;};
struct TransferFixture {
 std::vector<int> nodes_{1};struct {unsigned large_submit_failed{};} admission_diag_;bool submit{};
 bool SubmitGroup(int){return submit;}
 bool Run(Pass& pass,Loan& loan,std::shared_ptr<Job> job){int group=0;
#include "lease-transfer.inc"
  loan.index=DrawPool::Invalid;return true;
 }
};
int main(){
 validation::Pass valid;valid.draws.emplace_back();assert(validation::ValidatePass(valid));
 valid.draws[0].indexed=true;assert(!validation::ValidatePass(valid));valid.draws[0].indexed=false;
 valid.draws[0].vertex_buffer_count=1;assert(!validation::ValidatePass(valid));
 valid.draws[0].vertex_index=std::make_shared<validation::VI>();assert(!validation::ValidatePass(valid));
 valid.draws[0].vertex_buffer_count=0;assert(validation::ValidatePass(valid));
 std::array<int,3> source{11,22,33};auto original=source;
 std::array<Row,4> rows{{{0,1,source.data(),nullptr},{0,2,source.data()+1,nullptr},
                          {0,1,nullptr,source.data()},{0,2,nullptr,source.data()+1}}};
 Batch batch;
 std::array<std::vector<Row>,1> zero_groups{{std::vector<Row>{{0,0,source.data(),nullptr}}}};
 assert(batch.Check(zero_groups,1,source,source)==Batch::Admission::Unsupported);
 batch.Stage(rows,42);assert(source==original && batch.nw==4 && batch.nb==3 && batch.ni==3);
 for(unsigned i=0;i<4;++i)assert(batch.writes[i].dstSet==42);
 assert(batch.writes[0].pBufferInfo==batch.buffers.data() && batch.writes[1].pBufferInfo==batch.buffers.data()+1);
 assert(batch.writes[2].pImageInfo==batch.images.data() && batch.writes[3].pImageInfo==batch.images.data()+1);
 assert(batch.buffers[0]==11 && batch.buffers[1]==22 && batch.buffers[2]==33);
 assert(batch.images[0]==11 && batch.images[1]==22 && batch.images[2]==33);
 {Prepared abandoned;abandoned.pipeline_keepalive=std::make_shared<int>(3);assert(Prepare(abandoned));
  auto weak=std::weak_ptr<const void>(abandoned.pipeline_keepalive);
  try {Capture(&abandoned,[](const auto&){assert(false);return false;},true);assert(false);} catch(const std::bad_alloc&) {}
  assert(abandoned.vertex_index && !abandoned.vertex_index_published && !abandoned.vertex_index->pipeline_owner);
  abandoned.pipeline_keepalive.reset();assert(weak.expired());} // abort before publication adds no owner
 {Prepared pooled;pooled.pipeline_keepalive=std::make_shared<int>(8);auto weak=std::weak_ptr<const void>(pooled.pipeline_keepalive);
  Native::Parallel::SyncOwnerPins<1,256> completion;auto slot=completion.Reserve();
  assert(Prepare(pooled));{auto lease=Capture(&pooled,[&](const auto& owner){
   assert(pooled.vertex_index && pooled.vertex_index_published);return completion.Pin(slot,owner);});
   assert(!pooled.vertex_index);pooled.pipeline_keepalive.reset();assert(!weak.expired());}
  assert(!pooled.vertex_index && !weak.expired());assert(completion.Tag(slot,7));assert(completion.AssignPending(7));
  std::function<void()> cleanup;completion.Collect(7,[&](auto fn){cleanup=std::move(fn);return true;});
  assert(cleanup);cleanup();assert(weak.expired());} // no pool retention after GPU completion
 Prepared p; p.pipeline_keepalive=std::make_shared<int>(7); assert(Prepare(p));
 bool leaseable=true;assert(Eligible(&p,p.vertex_index.get(),false,leaseable) && !leaseable);
 p.vertex_index->attributes.push_back(1);assert(Eligible(&p,p.vertex_index.get(),false,leaseable) && leaseable);
 p.vertex_index->value=4;
 auto draw=Capture(&p); assert(p.vertex_index_published && !p.vertex_index && draw.vertex_index->value==4);
 auto old=draw.vertex_index; auto old_pipeline=std::weak_ptr<const void>(p.pipeline_keepalive); p.pipeline_keepalive.reset();
 std::atomic<bool> reader_ready{},reader_continue{};
 std::thread reader([old,&reader_ready,&reader_continue]{reader_ready.store(true,std::memory_order_release);
     while(!reader_continue.load(std::memory_order_acquire))std::this_thread::yield();
     assert(old->value==4 && old->pipeline_owner);});
 while(!reader_ready.load(std::memory_order_acquire))std::this_thread::yield();
 fail_alloc=true; assert(!Prepare(p)); assert(!p.vertex_index && p.vertex_index_published);
 fail_alloc=false; assert(Prepare(p)); assert(p.vertex_index!=old && !p.vertex_index_published); p.vertex_index->value=9;
 reader_continue.store(true,std::memory_order_release);reader.join();
 Pass pass;pass.draws.push_back(draw);
 {TransferFixture fixture;Loan loan;auto job=std::make_shared<Job>();
  assert(!fixture.Run(pass,loan,job));assert(pass.draws.size()==1 && pass.draws[0].vertex_index==old);
  assert(job->pass.draws.empty() && fixture.admission_diag_.large_submit_failed==1);}
 {TransferFixture fixture;fixture.submit=true;Loan loan;auto job=std::make_shared<Job>();
  Pass transferred=pass;assert(fixture.Run(transferred,loan,job));
  assert(transferred.draws.empty() && job->pass.draws[0].vertex_index==old);}
 auto copied=pass; auto moved=std::move(copied);assert(moved.draws[0].vertex_index==old);
 std::size_t bytes{};assert(MinimumPassBytes(pass,bytes));assert(bytes==sizeof(Pass)+pass.draws.capacity()*sizeof(Draw)+VertexLeaseCharge);
 Native::Parallel::SyncOwnerPins<1,256> pins;auto slot=pins.Reserve();assert(slot!=pins.Invalid);
 for(int i=0;i<256;++i)assert(pins.Pin(slot,draw.pipeline_owner));assert(!pins.Pin(slot,draw.pipeline_owner));
 pins.Queued(slot);assert(!old_pipeline.expired());
 auto synchronous=pins.Reserve();assert(pins.Pin(synchronous,draw.pipeline_owner));assert(pins.Tag(synchronous,11));
 assert(pins.AssignPending(11));std::function<void()> release;
 pins.Collect(10,[&](auto){assert(false);return false;});
 pins.Collect(11,[&](auto fn){release=std::move(fn);return true;});assert(release);release();
 auto cancelled=pins.Reserve();assert(pins.CancelEmpty(cancelled));
 Pass overflow;overflow.draws.fake_capacity=~std::size_t{0};assert(!MinimumPassBytes(overflow,bytes));
 Pass empty;empty.draws.emplace_back();assert(MinimumPassBytes(empty,bytes));assert(bytes==sizeof(Pass)+empty.draws.capacity()*sizeof(Draw));
}
