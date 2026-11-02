// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <vector>
#include "ryty_gpu/native/direct_coherence_registry.h"
#include "ryty_gpu/native/prepared_proof_context.h"
#include "ryty_gpu/native/prepared_direct_absent.h"
#include "ryty_gpu/native/prepared_direct_slot.h"
using u32=std::uint32_t;using u64=std::uint64_t;using VAddr=u64;
static bool fail_alloc=false;
void* operator new(size_t n){if(fail_alloc){fail_alloc=false;throw std::bad_alloc();}if(auto p=std::malloc(n))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}void operator delete(void* p,size_t)noexcept{std::free(p);}
struct VkBuffer_T{};
template<class T>T* Handle(std::uintptr_t value){return reinterpret_cast<T*>(value);}
namespace vk {
using Buffer=VkBuffer_T*;
enum class DescriptorType {eStorageBuffer,eSampledImage};
struct DescriptorBufferInfo{Buffer buffer{};u64 offset{},range{UINT64_MAX};};
struct Write{DescriptorType descriptorType{};u32 descriptorCount{};const DescriptorBufferInfo* pBufferInfo{};};
}
#define VK_WHOLE_SIZE UINT64_MAX
namespace Shader {
enum class BufferType {Guest,ClipPlanes,DirectTable};
struct Buffer {BufferType buffer_type{};bool is_written{},formatted{};u64 base{},bytes{};
    bool IsDirectEligible()const{return buffer_type==BufferType::Guest && !is_written && !formatted;}
    struct Sharp{u64 base_address{},size{};u64 GetSize()const{return size;}};
    Sharp GetSharp(const auto&)const{return {base,bytes};}};
struct Info{u64 identity{1};bool uses_direct{true};std::vector<Buffer> buffers;};
}
#define RYGT_COHERENCE_TEST
namespace VideoCore {
struct DirectMemory {u64 l1{10},zero{20};u64 L1Address()const{return l1;}u64 ZeroPageAddress()const{return zero;}};
Native::Parallel::DirectCoherenceRegistry<4,64> registry;
bool ParallelDirectViewsCoherent(const DirectMemory* owner,std::span<const std::pair<u64,u64>> ranges){return registry.CoversAll(owner,ranges);}
}
using Context=Native::Parallel::ProofProducerContext;
struct Action{bool is_buffer{true};size_t buffer_first{},stage_index{},stage_local{};u32 upload_match{},upload_size{};};
struct Upload{size_t info{},offset{},size{};int flat_stage{-1};};
struct StageSchema{u64 identity{1};size_t count{},table_local{3},table_upload{};};
struct Proof{bool descriptor_structure{true};std::array<StageSchema,1> stages{};std::array<u64,2> image_seal_mask{};u64 image_seal_epoch{};};
struct Bindings{std::vector<vk::DescriptorBufferInfo> buffers;std::vector<vk::Write> writes;};
struct Prepared {Proof cpu_proof;std::shared_ptr<Context> proof_context;u64 proof_mode_state{},unmap_gen{};Bindings bindings;
    std::vector<Action> descriptor_actions;std::vector<Upload> uploads;std::vector<unsigned char> upload_bytes;
    bool buffer_prefix_sealed{},buffer_prefix_clip{};const VideoCore::DirectMemory* buffer_prefix_direct{};u64 buffer_prefix_l1{},buffer_prefix_zero{};
    std::vector<std::pair<VAddr,VAddr>> buffer_prefix_ranges;};
struct Pipeline {std::array<const Shader::Info*,1> canonical{};const auto& CanonicalStages()const{return canonical;}};
struct Regs {struct {bool user_clip_plane_enable{};} clipper_control;};
bool ParallelReadonlyDescriptorEnabled(){return true;}
struct TestBuffer {bool mapped{true};std::uintptr_t native{77};u64 bytes{4096};bool Mapped()const{return mapped;}std::uintptr_t NativeHandle()const{return native;}u64 Size()const{return bytes;}};
struct Stream {struct Chunk{TestBuffer* buffer{};};std::vector<Chunk> chunks;};
struct Parallel {std::shared_ptr<Context> prepared_proof_context;u64 buffer_prefix_attempts{},buffer_prefix_accepts{},buffer_prefix_fallbacks{},buffer_prefix_coherence_misses{},buffer_prefix_invalidations{},image_batch_invalidations{},image_batch_draws{},image_batch_infos{};};
struct Owner {
    struct {struct {VideoCore::DirectMemory* direct{};VideoCore::DirectMemory* GetDirectMemory()const{return direct;}} buffer_cache;
            struct {bool enabled{true};bool GetDevice()const{return enabled;}} instance;} r;
    std::atomic<u64> unmap_generation{0};struct {u64 seq{};u64 Seq()const{return seq;}} buffer_log;
    std::atomic<u64> sampler_cache_epoch{1};
    void Produce(Prepared* p,Pipeline& pipeline,std::span<const Shader::Info* const> stages,const Regs& regs) {
#include "buffer-prefix-producer.inc"
    }
    bool Recorder(Prepared* prepared,Parallel* parallel,const Regs& regs,const Stream& s,
                  bool known_structure=true,bool known_actions=true,bool known_stage_schema=true,
                  bool from_prepared=true,bool buffer_freshness_certified=true,u64 buffer_freshness_seq=0,bool trusted=true) {
        const auto& b=prepared->bindings;
#include "buffer-prefix-recorder.inc"
        return buffer_prefix;
    }
    bool Final(Prepared* prepared,Parallel* parallel,bool buffer_prefix,bool descriptor_safe,u32& descriptor_fail_reason) {
        size_t image_batch_infos=0;auto* worker_result=&prepared->cpu_proof;bool buffer_freshness_certified=true;u64 buffer_freshness_seq=0;
#include "buffer-prefix-end.inc"
        return descriptor_safe;
    }
    std::pair<size_t,size_t> Loop(Prepared* prepared,bool buffer_prefix) {
        const auto& b=prepared->bindings;const bool known_actions=true,image_batch=true;auto* worker_result=&prepared->cpu_proof;
        size_t image_batch_infos=0,unsealed=0;
#include "buffer-prefix-loop.inc"
            (void)action;
            if(write.descriptorType==vk::DescriptorType::eStorageBuffer)continue;
#include "buffer-prefix-image-skip.inc"
            ++unsealed;
        }
        return {image_batch_infos,unsealed};
    }
};
static void Words(std::vector<unsigned char>& out,size_t word,std::array<u32,4> value){std::memcpy(out.data()+word*4,value.data(),16);}
int main() {
#ifdef _WIN32
    _putenv_s("RYGT_PARALLEL_ABSENT_GUEST","1");
#else
    setenv("RYGT_PARALLEL_ABSENT_GUEST","1",1);
#endif
    Owner owner;VideoCore::DirectMemory direct;owner.r.buffer_cache.direct=&direct;
    VideoCore::registry.Register(&direct,true);VideoCore::registry.Add(&direct,100,200,true);
    Shader::Info stage;stage.buffers.resize(4);stage.buffers[2].buffer_type=Shader::BufferType::ClipPlanes;
    stage.buffers[3].buffer_type=Shader::BufferType::DirectTable;Pipeline pipeline{{&stage}};
    std::array<const Shader::Info*,1> stages{&stage};Regs regs;Prepared p;
    p.proof_context=std::make_shared<Context>();p.proof_context->SetMode(true);p.proof_mode_state=p.proof_context->state.load();
    p.cpu_proof.stages[0].count=4;p.bindings.buffers.resize(4);p.bindings.buffers[3].range=80;
    p.bindings.writes.resize(4);p.descriptor_actions.resize(4);
    for(size_t i=0;i<4;++i){p.bindings.writes[i]={vk::DescriptorType::eStorageBuffer,1,&p.bindings.buffers[i]};p.descriptor_actions[i].buffer_first=i;p.descriptor_actions[i].stage_local=i;}
    p.uploads.push_back({3,0,80,-1});p.descriptor_actions[3].upload_match=1;p.descriptor_actions[3].upload_size=80;
    p.upload_bytes.resize(80);Words(p.upload_bytes,0,{10,0,20,0});Words(p.upload_bytes,4,{100,0,100,1});
    Parallel parallel{p.proof_context};TestBuffer backing;Stream stream{{{&backing}}};
    const auto reset=[&]{p.buffer_prefix_sealed=false;p.buffer_prefix_clip=false;p.buffer_prefix_direct=nullptr;p.buffer_prefix_ranges.clear();};
    owner.Produce(&p,pipeline,stages,regs);assert(p.buffer_prefix_sealed && p.buffer_prefix_clip && p.buffer_prefix_ranges.size()==1);
    p.bindings.buffers[3]={Handle<VkBuffer_T>(77),0,80};
    assert(owner.Recorder(&p,&parallel,regs,stream));u32 reason=0;assert(owner.Final(&p,&parallel,true,true,reason));
    const auto table=std::span<const std::byte>{reinterpret_cast<const std::byte*>(p.upload_bytes.data()),p.upload_bytes.size()};
    const auto absent=stage.buffers[1].GetSharp(stage);
    assert(Native::Parallel::IsPreparedDirectSlot(table,4,0,true,true,0,VK_WHOLE_SIZE));
    assert(Native::Parallel::IsPreparedAbsentDirectSlot(table,4,1,true,true,true,0,VK_WHOLE_SIZE,absent.base_address,absent.GetSize()));
    assert(!owner.Recorder(&p,&parallel,regs,stream,false));assert(!owner.Recorder(&p,&parallel,regs,stream,true,false));assert(!owner.Recorder(&p,&parallel,regs,stream,true,true,false));
    assert(!owner.Recorder(&p,&parallel,regs,stream,true,true,true,true,false));
    assert(!owner.Recorder(&p,&parallel,regs,stream,true,true,true,false));
    assert(!owner.Recorder(&p,&parallel,regs,stream,true,true,true,true,true,0,false));
    {p.bindings.writes.push_back({vk::DescriptorType::eSampledImage,1,nullptr});p.bindings.writes.push_back({vk::DescriptorType::eSampledImage,1,nullptr});p.descriptor_actions.resize(6);
     p.cpu_proof.image_seal_mask[0]=u64{1}<<4;assert((owner.Loop(&p,true)==std::pair<size_t,size_t>{1,1}));
     p.bindings.writes.resize(4);p.descriptor_actions.resize(4);p.cpu_proof.image_seal_mask={};}
    regs.clipper_control.user_clip_plane_enable=true;assert(!owner.Recorder(&p,&parallel,regs,stream));regs.clipper_control.user_clip_plane_enable=false;
    direct.l1++;assert(!owner.Recorder(&p,&parallel,regs,stream));direct.l1--;
    backing.mapped=false;assert(!owner.Recorder(&p,&parallel,regs,stream));backing.mapped=true;
    VideoCore::registry.Remove(&direct,100,200);assert(!owner.Recorder(&p,&parallel,regs,stream));VideoCore::registry.Add(&direct,100,200,true);
    ++owner.unmap_generation;assert(!owner.Recorder(&p,&parallel,regs,stream));--owner.unmap_generation;
    ++owner.buffer_log.seq;assert(!owner.Recorder(&p,&parallel,regs,stream));reason=0;assert(!owner.Final(&p,&parallel,true,true,reason) && reason==10);--owner.buffer_log.seq;
    ++owner.unmap_generation;reason=0;assert(!owner.Final(&p,&parallel,true,true,reason) && reason==10);--owner.unmap_generation;
    reason=3;assert(!owner.Final(&p,&parallel,true,false,reason) && reason==3);
    p.proof_context->SetMode(false);reason=0;assert(!owner.Final(&p,&parallel,true,true,reason));
    p.proof_context->SetMode(true);p.proof_mode_state=p.proof_context->state.load();
    p.bindings.buffers[3]={nullptr,0,80};reset();Words(p.upload_bytes,4,{0,0,0,2});owner.Produce(&p,pipeline,stages,regs);assert(!p.buffer_prefix_sealed);
    Words(p.upload_bytes,4,{100,0,100,1});reset();p.uploads.push_back(p.uploads[0]);owner.Produce(&p,pipeline,stages,regs);assert(!p.buffer_prefix_sealed);p.uploads.pop_back();
    reset();stage.buffers[0].formatted=true;owner.Produce(&p,pipeline,stages,regs);assert(!p.buffer_prefix_sealed);stage.buffers[0].formatted=false;
    {Shader::Info other=stage;other.identity=2;pipeline.canonical[0]=&other;reset();owner.Produce(&p,pipeline,stages,regs);assert(!p.buffer_prefix_sealed);pipeline.canonical[0]=&stage;}
    reset();p.uploads[0].offset=SIZE_MAX;owner.Produce(&p,pipeline,stages,regs);assert(!p.buffer_prefix_sealed);p.uploads[0].offset=0;
    reset();Words(p.upload_bytes,4,{UINT32_MAX,UINT32_MAX,64,1});owner.Produce(&p,pipeline,stages,regs);assert(!p.buffer_prefix_sealed);Words(p.upload_bytes,4,{100,0,100,1});
    reset();std::vector<std::pair<VAddr,VAddr>>{}.swap(p.buffer_prefix_ranges);fail_alloc=true;owner.Produce(&p,pipeline,stages,regs);assert(!p.buffer_prefix_sealed && p.buffer_prefix_ranges.empty());
}
