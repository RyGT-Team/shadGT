// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include "ryty_gpu/native/descriptor_snapshot.h"
#include <cassert>
#include <cstdio>
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
 puts("PASS real Snapshot patch: mixed, multidraw rows, null buffer, mismatch no mutation and zero buffers");
}
