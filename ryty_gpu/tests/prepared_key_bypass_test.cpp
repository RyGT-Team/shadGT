// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/prepared_descriptor_key.h"
#include "video_core/renderer_vulkan/descriptor_set_memo.h"
#define XXH_INLINE_ALL
#include "ryty_gpu/native/adaptive_xxh3.h"
#include <cassert>
#include <array>
#include <vector>
using u64=std::uint64_t;using u32=std::uint32_t;
extern "C" int NativeXXH512Available(){return 0;}
extern "C" std::uint64_t NativeXXH512Long(const void* p,std::size_t n){return XXH3_64bits(p,n);}
extern "C" std::uint64_t NativeXXH512LongSeed(const void* p,std::size_t n,std::uint64_t seed){return XXH3_64bits_withSeed(p,n,seed);}
struct Pipeline {vk::PipelineLayout layout{};vk::DescriptorSetLayout set{};vk::PipelineLayout GetLayout()const{return layout;}vk::DescriptorSetLayout SetLayout(u32)const{return set;}u32 PushSet()const{return 3;}};
struct NativeRenderer {struct DrawBindings {bool groups_ready=true;std::array<std::vector<vk::WriteDescriptorSet>,3> grouped_writes;std::array<Vulkan::PreparedDescriptorKey,3> descriptor_keys;vk::PipelineLayout key_layout{};void PrepareWriteGroups(){groups_ready=true;}void PrepareDescriptorKeys(const Pipeline&);};};
#include "prepared-key-build-under-test.inc"
static void RebuildIfNeeded(NativeRenderer::DrawBindings& b,const Pipeline& pipeline){
#include "prepared-key-layout-under-test.inc"
}
struct Prepared {std::vector<int> uploads;};
static bool Gate(bool from_prepared,Prepared* prepared,const std::vector<int>& feedback){
#include "prepared-key-guard-under-test.inc"
 return prepared_buffer_keys_unchanged;
}
static bool Refresh(Vulkan::PreparedDescriptorKey& prepared_key,bool prepared_buffer_keys_unchanged,u64& descriptor_buffer_key_skipped,u64& descriptor_buffer_key_scanned){
#include "prepared-key-refresh-under-test.inc"
 return key_changed;
}
int main(){
 auto handle=[](std::uintptr_t n){return vk::Buffer((VkBuffer)n);};
 vk::DescriptorBufferInfo info{handle(10),4,32};vk::DescriptorImageInfo image{};
 std::array<vk::WriteDescriptorSet,2> rows{};rows[0].descriptorType=vk::DescriptorType::eStorageBuffer;rows[0].descriptorCount=1;rows[0].pBufferInfo=&info;
 rows[1].descriptorType=vk::DescriptorType::eSampledImage;rows[1].descriptorCount=1;rows[1].pImageInfo=&image;
 auto layout=vk::DescriptorSetLayout((VkDescriptorSetLayout)(std::uintptr_t)1);
 Vulkan::PreparedDescriptorKey key;key.Build<false>(layout,rows);auto original=key.words;
 Prepared p;std::vector<int> feedback;u64 skipped=0,scanned=0;
 assert(Gate(true,&p,feedback));assert(!Refresh(key,Gate(true,&p,feedback),skipped,scanned));assert(key.words==original && skipped==1 && scanned==0);
 // Old refresh and new bypass have identical complete key bytes when unchanged.
 assert(!key.RefreshBuffers<false>() && key.words==original);
 info.buffer=handle(11);p.uploads.push_back(1);assert(!Gate(true,&p,feedback));assert(Refresh(key,Gate(true,&p,feedback),skipped,scanned));assert(key.words!=original && scanned==1);
 auto current=key.words;p.uploads.clear();feedback.push_back(1);assert(!Gate(true,&p,feedback));assert(!Refresh(key,Gate(true,&p,feedback),skipped,scanned));assert(key.words==current);
 assert(!Gate(false,nullptr,{}));feedback.clear();
 // Layout mismatch rebuild happens first and yields current values, then bypass is safe.
 auto layout2=vk::DescriptorSetLayout((VkDescriptorSetLayout)(std::uintptr_t)2);
 NativeRenderer::DrawBindings b;b.grouped_writes[0]={rows[0]};b.grouped_writes[1]={rows[1]};
 Pipeline pipeline{vk::PipelineLayout((VkPipelineLayout)(std::uintptr_t)2),layout2};RebuildIfNeeded(b,pipeline);
 for(unsigned i=0;i<2;++i){auto& k=b.descriptor_keys[i];auto bytes=k.words;auto hash=k.hash;assert(hash==Native::Parallel::AdaptiveXXH3(bytes.data(),bytes.size()*sizeof(u64)));assert(!Refresh(k,Gate(true,&p,feedback),skipped,scanned));assert(k.words==bytes && k.hash==hash);}
 assert(b.key_layout==pipeline.GetLayout());key.Build<false>(layout2,rows);auto rebuilt=key.words;assert(rebuilt!=original);assert(!Refresh(key,Gate(true,&p,feedback),skipped,scanned));assert(key.words==rebuilt);
 // The guard excludes nonprepared paths; the default argument is source-reviewed.
 assert(!Gate(false,nullptr,{}));
 auto bad=rows;int token=0;bad[0].pNext=&token;key.Build<false>(layout2,bad);assert(!key.cacheable);assert(!Refresh(key,true,skipped,scanned));assert(!key.cacheable);
 Vulkan::DescriptorSetMemo<unsigned,1> memo;const std::array<u64,3> a{1,2,3},collision_key{1,2,4};memo.Store(7,a,9);assert(memo.Find(7,a)==9 && memo.Find(7,collision_key)==0);
}
