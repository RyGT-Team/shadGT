// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include "ryty_gpu/native/descriptor_snapshot.h"
#include <cassert>
#include <cstdio>
#include <future>
#include <stdexcept>
int main(){
 using namespace vk;std::array<DescriptorBufferInfo,2> infos{{{Buffer{reinterpret_cast<VkBuffer>(1)},4,8},{Buffer{reinterpret_cast<VkBuffer>(2)},12,16}}};
 std::array<DescriptorImageInfo,1> images{{{Sampler{},ImageView{reinterpret_cast<VkImageView>(3)},ImageLayout::eGeneral}}};
 std::array<WriteDescriptorSet,3> rows{{{.dstBinding=0,.descriptorCount=1,.descriptorType=DescriptorType::eStorageBuffer,.pBufferInfo=&infos[0]}, {.dstBinding=1,.descriptorCount=1,.descriptorType=DescriptorType::eSampledImage,.pImageInfo=&images[0]}, {.dstBinding=2,.descriptorCount=1,.descriptorType=DescriptorType::eStorageBuffer,.pBufferInfo=&infos[1]}}};
 auto made=DescriptorSnapshot::Make(rows,infos,images,65536);assert(made);auto& snap=*made;
 std::array<DescriptorBufferInfo,2> replacement{{{Buffer{reinterpret_cast<VkBuffer>(4)},20,24},{Buffer{},0,0}}};
 assert(!snap.PatchBufferBindings(std::span<const DescriptorBufferInfo>{replacement.data(),1}));assert(snap.Writes()[0].pBufferInfo->buffer==infos[0].buffer);
 assert(snap.PatchBufferBindings(replacement));assert(snap.Writes()[0].pBufferInfo->buffer==replacement[0].buffer);assert(snap.Writes()[2].pBufferInfo->buffer==replacement[1].buffer);assert(snap.Writes()[1].pImageInfo->imageView==images[0].imageView);
 std::array<WriteDescriptorSet,1> image_only{rows[1]};auto image=DescriptorSnapshot::Make(image_only,infos,images,65536);assert(image && image->PatchBufferBindings({}));
 std::array<WriteDescriptorSet,1> multi{{{.dstBinding=0,.descriptorCount=2,.descriptorType=DescriptorType::eStorageBuffer,.pBufferInfo=infos.data()}}};auto m=DescriptorSnapshot::Make(multi,infos,images,65536);assert(m && m->PatchBufferBindings(replacement));assert(m->Writes()[0].pBufferInfo[1].buffer==replacement[1].buffer);
 auto owned_snapshot=DescriptorSnapshot::Make(rows,infos,images,65536);assert(owned_snapshot);
 OwnedDescriptorUpdates updates{std::move(*owned_snapshot)};
 const auto expected=infos[0].buffer;infos[0].buffer=Buffer{};images[0].imageView=ImageView{};
 unsigned calls=0;
 try {updates.Ensure([&](auto){++calls;throw std::runtime_error("failed update");});assert(false);}catch(const std::runtime_error&) {}
 assert(!updates.initialized && calls==1);
 auto worker=std::async(std::launch::async,[&]{updates.Ensure([&](auto writes){++calls;assert(writes[0].pBufferInfo->buffer==expected);assert(writes[1].pImageInfo->imageView);});});
 worker.get();assert(updates.initialized && calls==2);
 updates.Ensure([&](auto){++calls;});assert(calls==2); // fallback after worker completion
 // Inject failures before initialization, after a bind, midway through a
 // stream, and after the last bind. The same objects survive into fallback.
 for(unsigned fail_after:{0u,1u,2u,3u}) {
  std::array<std::unique_ptr<OwnedDescriptorUpdates>,3> packets;
  for(auto& packet:packets){auto s=DescriptorSnapshot::Make(rows,infos,images,65536);assert(s);packet=std::make_unique<OwnedDescriptorUpdates>(std::move(*s));}
  std::array<unsigned,3> counts{};
  auto replay=[&](unsigned i){packets[i]->Ensure([&](auto){++counts[i];});};
  auto attempt=std::async(std::launch::async,[&]{for(unsigned i=0;i<fail_after;++i)replay(i);});attempt.get();
  for(unsigned i=0;i<3;++i)replay(i);
  for(auto count:counts)assert(count==1);
 }
 puts("PASS real Snapshot patch: mixed, multidraw rows, null buffer, mismatch no mutation and zero buffers");
}
