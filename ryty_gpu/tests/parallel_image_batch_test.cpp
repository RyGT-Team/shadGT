// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <vector>
#include "ryty_gpu/native/prepared_sampled_view_proof.h"
#include "ryty_gpu/native/prepared_proof_context.h"
#include "src/video_core/renderer_vulkan/parallel_absent_sampled_image.h"
using u32=std::uint32_t;using u64=std::uint64_t;
using u8=std::uint8_t;
bool ParallelProofPhaseEnabled(){return true;}
namespace vk {
using ImageView=std::uintptr_t;using Sampler=std::uintptr_t;
enum class ImageLayout { eGeneral,eOther };
enum class DescriptorType { eUniformBuffer,eStorageBuffer,eSampledImage,eSampler,eStorageImage };
struct DescriptorImageInfo {Sampler sampler{};ImageView imageView{};ImageLayout imageLayout{};};
struct Write {DescriptorType descriptorType{};const void *pNext{},*pTexelBufferView{},*pBufferInfo{};const DescriptorImageInfo* pImageInfo{};u32 descriptorCount{};};
}
std::atomic<u64> sampler_cache_epoch{1};
struct DescriptorSnapshot {
#include "image-batch-inside.inc"
    static bool ImageSliceValid(const vk::DescriptorImageInfo* p,size_t n,std::span<const vk::DescriptorImageInfo> s) {
        return Inside(p,n,s);
    }
};
using Context=Native::Parallel::ProofProducerContext;
struct Entries {
    std::vector<const void*> values;bool change_epoch{};
    size_t size()const{return values.size();}
    const void* operator[](size_t i)const {if(change_epoch)++sampler_cache_epoch;return values[i];}
};
struct Origin {size_t slot{};vk::Sampler handle{};};
struct Proof {bool descriptor_structure{};std::array<u64,2> image_seal_mask{};u64 image_seal_epoch{};const void* image_seal_owner{};};
struct Bindings {std::vector<vk::DescriptorImageInfo> images;std::vector<vk::Write> writes;Entries sampled;};
struct Prepared {Proof cpu_proof;std::shared_ptr<Context> proof_context;bool sampler_origins_valid{true};u64 sampler_origins_epoch{1},proof_mode_state{1};Bindings bindings;std::vector<Origin> sampler_origins;std::vector<Native::Parallel::SampledViewProof<vk::ImageView>> sampled_view_proofs;};
struct Parallel {std::shared_ptr<Context> prepared_proof_context;u64 image_batch_invalidations{},image_batch_draws{},image_batch_infos{};};
struct Owner {
    vk::Sampler next_sampler{17};bool change_during_factory{};
    vk::Sampler SamplerFor(int,int,bool) {if(change_during_factory)++sampler_cache_epoch;return next_sampler;}
    void Capture(Prepared& p) {
        auto& b=p.bindings;const int ssharp=0;struct {int ta_bc_base{};} regs;struct {bool is_depth{};} sampler;
#define VK_NULL_HANDLE 0
#include "image-batch-factory.inc"
#undef VK_NULL_HANDLE
    }
    void Seal(Prepared* p) {
#include "image-batch-producer.inc"
    }
    bool Start(Prepared* prepared,Parallel* parallel,bool known_structure=true,bool has_feedback=false) {
        auto* worker_result=&prepared->cpu_proof;
        std::vector<int> feedback;if(has_feedback)feedback.push_back(1);
#include "image-batch-start.inc"
        return image_batch;
    }
    bool End(Prepared* prepared,Parallel* parallel,size_t image_batch_infos,bool descriptor_safe,u32& descriptor_fail_reason) {
        auto* worker_result=&prepared->cpu_proof;
#include "image-batch-end.inc"
        return descriptor_safe;
    }
};
struct NullStage {
    struct Image {bool is_written{};u32 NumBindings(const NullStage&)const{return 1;} struct Sharp {u64 Address()const{return 0;}};Sharp GetSharp(const NullStage&)const{return {};}};
    std::vector<Image> images{{}};
};
struct RowResult {size_t skipped{},fallback{},null_proofs{};};
RowResult CheckRows(Prepared* prepared,bool image_batch) {
    auto* worker_result=&prepared->cpu_proof;size_t action_index=0,image_batch_infos=0;RowResult result;
    const NullStage stage{};const std::array<const NullStage*,1> stages{&stage};
    for(const auto& write:prepared->bindings.writes) {
        ++action_index;
        if(write.descriptorType==vk::DescriptorType::eUniformBuffer || write.descriptorType==vk::DescriptorType::eStorageBuffer)continue;
#include "image-batch-skip.inc"
        ++result.fallback;
        if(write.descriptorType==vk::DescriptorType::eSampledImage && !write.pImageInfo->imageView) {
            assert(Native::Parallel::AbsentSampledImage<NullStage>(stages,0,0,0,1,
                [](const auto&){return 0;},[](const auto&){return 0;}));
            ++result.null_proofs;
        }
    }
    result.skipped=image_batch_infos;return result;
}
int main() {
    int image{};Owner owner;Prepared p;p.proof_context=std::make_shared<Context>();p.proof_context->SetMode(true);
    p.proof_mode_state=p.proof_context->state.load();p.cpu_proof.descriptor_structure=true;
    p.bindings.images={{0,41,vk::ImageLayout::eGeneral},{17,0,vk::ImageLayout::eGeneral}};
    p.bindings.sampled.values={&image};
    p.bindings.writes={{vk::DescriptorType::eSampledImage,nullptr,nullptr,nullptr,&p.bindings.images[0],1},
                       {vk::DescriptorType::eSampler,nullptr,nullptr,nullptr,&p.bindings.images[1],1}};
    assert(Native::Parallel::AddSampledViewProof(p.sampled_view_proofs,0,0,vk::ImageView{41},&image,1,false));
    p.sampler_origins={{1,17}};Parallel parallel{p.proof_context};
    const auto reset=[&]{p.cpu_proof.image_seal_mask={};};
    owner.Seal(&p);assert(p.cpu_proof.image_seal_mask[0]==3);
    assert(owner.Start(&p,&parallel));u32 reason=0;assert(owner.End(&p,&parallel,2,true,reason));
    assert(!owner.Start(&p,&parallel,false));assert(!owner.Start(&p,&parallel,true,true));
    Owner different_owner;assert(!different_owner.Start(&p,&parallel));
    Parallel different_context{std::make_shared<Context>()};assert(!owner.Start(&p,&different_context));
    p.sampler_origins[0].handle=18;reset();owner.Seal(&p);assert(p.cpu_proof.image_seal_mask[0]==1);p.sampler_origins[0].handle=17;
    p.sampler_origins[0].slot=0;reset();owner.Seal(&p);assert(p.cpu_proof.image_seal_mask[0]==1);p.sampler_origins[0].slot=1;
    p.sampler_origins_epoch=2;reset();owner.Seal(&p);assert(p.cpu_proof.image_seal_mask[0]==1);p.sampler_origins_epoch=1;
    p.sampler_origins_valid=false;reset();owner.Seal(&p);assert(!p.cpu_proof.image_seal_mask[0]);p.sampler_origins_valid=true;
    p.cpu_proof.descriptor_structure=false;reset();owner.Seal(&p);assert(!p.cpu_proof.image_seal_mask[0]);p.cpu_proof.descriptor_structure=true;
    {auto writes=std::move(p.bindings.writes);reset();owner.Seal(&p);assert(!p.cpu_proof.image_seal_mask[0]);p.bindings.writes=std::move(writes);}
    p.bindings.images[1].sampler=0;reset();owner.Seal(&p);assert(p.cpu_proof.image_seal_mask[0]==1);p.bindings.images[1].sampler=17;
    p.bindings.images[1].imageView=1;reset();owner.Seal(&p);assert(p.cpu_proof.image_seal_mask[0]==1);p.bindings.images[1].imageView=0;
    p.bindings.images[0].imageView=42;reset();owner.Seal(&p);assert(p.cpu_proof.image_seal_mask[0]==2);p.bindings.images[0].imageView=41;
    p.bindings.images[0].imageView=0;reset();owner.Seal(&p);assert(p.cpu_proof.image_seal_mask[0]==2);
    {const auto rows=CheckRows(&p,owner.Start(&p,&parallel));assert(rows.skipped==1 && rows.fallback==1 && rows.null_proofs==1);}
    p.bindings.images[0].imageView=41;
    {auto writes=p.bindings.writes;const auto sampler=writes[1];
     p.bindings.writes.assign(128,vk::Write{vk::DescriptorType::eStorageBuffer});
     for(auto i:{0,63,64,127})p.bindings.writes[i]=sampler;
     reset();owner.Seal(&p);assert(p.cpu_proof.image_seal_mask[0]==(u64{1}|(u64{1}<<63)));
     assert(p.cpu_proof.image_seal_mask[1]==(u64{1}|(u64{1}<<63)));
     const auto rows=CheckRows(&p,owner.Start(&p,&parallel));assert(rows.skipped==4 && rows.fallback==0);
     p.bindings.writes=std::move(writes);}
    {const auto saved_image=p.bindings.images[1];p.bindings.images[1]={0,0,vk::ImageLayout::eGeneral};
     p.bindings.writes[0].descriptorCount=2;reset();owner.Seal(&p);assert((p.cpu_proof.image_seal_mask[0]&1)==0);
     p.bindings.images[1]={0,42,vk::ImageLayout::eGeneral};
     assert(Native::Parallel::AddSampledViewProof(p.sampled_view_proofs,1,0,vk::ImageView{42},&image,1,false));
     reset();owner.Seal(&p);assert((p.cpu_proof.image_seal_mask[0]&1)!=0);
     p.bindings.writes[0].descriptorCount=1;p.bindings.images[1]=saved_image;}
    {auto saved=p.proof_context;p.proof_context.reset();reset();owner.Seal(&p);assert(!p.cpu_proof.image_seal_mask[0]);p.proof_context=std::move(saved);}
    p.bindings.writes[0].descriptorType=vk::DescriptorType::eStorageImage;reset();owner.Seal(&p);assert(p.cpu_proof.image_seal_mask[0]==2);p.bindings.writes[0].descriptorType=vk::DescriptorType::eSampledImage;
    p.bindings.writes[0].pImageInfo=p.bindings.images.data()+p.bindings.images.size();reset();owner.Seal(&p);assert(p.cpu_proof.image_seal_mask[0]==2);p.bindings.writes[0].pImageInfo=&p.bindings.images[0];
    p.bindings.sampled.change_epoch=true;reset();owner.Seal(&p);assert(!p.cpu_proof.image_seal_mask[0]);p.bindings.sampled.change_epoch=false;sampler_cache_epoch=1;
    {auto writes=p.bindings.writes;p.bindings.writes.resize(129);reset();owner.Seal(&p);assert(!p.cpu_proof.image_seal_mask[0] && !p.cpu_proof.image_seal_mask[1]);p.bindings.writes=std::move(writes);}
    reset();owner.Seal(&p);assert(owner.Start(&p,&parallel));sampler_cache_epoch=2;reason=0;
    const bool snapshot_or_packet=owner.End(&p,&parallel,true,true,reason);
    assert(!snapshot_or_packet && reason==10);
    sampler_cache_epoch=1;reason=3;assert(!owner.End(&p,&parallel,true,false,reason) && reason==3); // buffer failure stays rejected
    p.proof_context->SetMode(false);p.proof_context->SetMode(true);
    assert(!owner.Start(&p,&parallel));reason=0;assert(!owner.End(&p,&parallel,true,true,reason) && reason==10);
    p.proof_mode_state=p.proof_context->state.load();reset();owner.Seal(&p);assert(owner.Start(&p,&parallel));
    p.proof_context->closing.store(true);reason=0;assert(!owner.End(&p,&parallel,true,true,reason) && reason==10);
    {const auto s=std::span<const vk::DescriptorImageInfo>{p.bindings.images};
     assert(!DescriptorSnapshot::ImageSliceValid(nullptr,1,s));
     assert(!DescriptorSnapshot::ImageSliceValid(s.data(),0,s));
     assert(!DescriptorSnapshot::ImageSliceValid(s.data()+s.size(),1,s));
     assert(DescriptorSnapshot::ImageSliceValid(s.data()+1,1,s));
     const auto bad=reinterpret_cast<const vk::DescriptorImageInfo*>(reinterpret_cast<std::uintptr_t>(s.data())+1);
     assert(!DescriptorSnapshot::ImageSliceValid(bad,1,s));}
    auto make_prepared=[] {Prepared value;value.proof_context=std::make_shared<Context>();value.proof_context->SetMode(true);value.sampler_origins.reserve(80);return value;};
    sampler_cache_epoch=1;
    {auto x=make_prepared();owner.Capture(x);assert(x.sampler_origins_valid && x.sampler_origins.size()==1 && x.sampler_origins[0].slot==0 && x.sampler_origins[0].handle==17 && x.bindings.images[0].sampler==17);}
    {auto x=make_prepared();owner.next_sampler=0;owner.Capture(x);assert(!x.sampler_origins_valid && x.sampler_origins.empty());owner.next_sampler=17;}
    {auto x=make_prepared();owner.change_during_factory=true;owner.Capture(x);assert(!x.sampler_origins_valid && x.sampler_origins.empty());owner.change_during_factory=false;sampler_cache_epoch=1;}
    {auto x=make_prepared();for(int i=0;i<80;++i)owner.Capture(x);assert(x.sampler_origins_valid && x.sampler_origins.size()==80);owner.Capture(x);assert(!x.sampler_origins_valid && x.sampler_origins.size()==80);}
    {auto x=make_prepared();owner.Capture(x);++sampler_cache_epoch;owner.Capture(x);assert(!x.sampler_origins_valid && x.sampler_origins.size()==1);}
}
