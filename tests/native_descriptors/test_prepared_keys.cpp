// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/prepared_descriptor_key.h"
#include "video_core/renderer_vulkan/descriptor_set_memo.h"
#include <array>
#include <stdexcept>
#include <iostream>
using U=std::uint64_t;
struct Reference { std::vector<U> words; bool cacheable=true; };
Reference BuildReference(vk::DescriptorSetLayout layout,std::span<const vk::WriteDescriptorSet> writes) {
    Reference r;r.words.push_back(U(VkDescriptorSetLayout(layout)));
    for(const auto& w:writes) {
        r.words.insert(r.words.end(),{U(w.dstBinding),U(w.dstArrayElement),U(w.descriptorCount),U(w.descriptorType)});
        if(w.pNext||w.pTexelBufferView){r.cacheable=false;break;}
        if(w.pBufferInfo)for(unsigned i=0;i<w.descriptorCount;++i){const auto& b=w.pBufferInfo[i];r.words.insert(r.words.end(),{U(VkBuffer(b.buffer)),b.offset,b.range});}
        else if(w.pImageInfo)for(unsigned i=0;i<w.descriptorCount;++i){const auto& b=w.pImageInfo[i];r.words.insert(r.words.end(),{U(VkSampler(b.sampler)),U(VkImageView(b.imageView)),U(b.imageLayout)});}
        else {r.cacheable=false;break;}
    }
    return r;
}
int main() {
    Vulkan::PreparedDescriptorKey key;unsigned cases=0;
    std::array<bool,256> buckets{};
    for(unsigned count=0;count<=24;++count)for(unsigned pattern=0;pattern<16;++pattern)for(unsigned n=1;n<=4;++n) {
        std::array<vk::DescriptorBufferInfo,32> buffers{};
        std::array<vk::DescriptorImageInfo,32> images{};
        for(unsigned i=0;i<32;++i){buffers[i]=vk::DescriptorBufferInfo{vk::Buffer((VkBuffer)(uintptr_t)(i+1)),i*256,512};images[i]=vk::DescriptorImageInfo{vk::Sampler((VkSampler)(uintptr_t)(i+100)),vk::ImageView((VkImageView)(uintptr_t)(i+200)),vk::ImageLayout::eGeneral};}
        std::vector<vk::WriteDescriptorSet> writes;
        for(unsigned i=0;i<count;++i) {
            vk::WriteDescriptorSet w{};w.dstBinding=i;w.dstArrayElement=i%3;w.descriptorCount=n;
            if((i+pattern)%3==0){w.descriptorType=vk::DescriptorType::eStorageBuffer;w.pBufferInfo=&buffers[i];}
            else {w.descriptorType=vk::DescriptorType::eCombinedImageSampler;w.pImageInfo=&images[i];}
            writes.push_back(w);
        }
        const auto layout=vk::DescriptorSetLayout((VkDescriptorSetLayout)(uintptr_t)(pattern+1));
        key.Build(layout,writes);
        for(unsigned commit=0;commit<4;++commit) {
            if(commit)for(auto& b:buffers){b.buffer=vk::Buffer((VkBuffer)(uintptr_t)(U(VkBuffer(b.buffer))+100));b.offset+=64;b.range+=8;}
            key.RefreshBuffers<false>();const auto expected=BuildReference(layout,writes);
            if(key.words!=expected.words)throw std::runtime_error("dense-key refresh is stale");
            key.RefreshBuffers();
            if(key.cacheable!=expected.cacheable||key.words!=expected.words)throw std::runtime_error("key differs from original after ordered commit");
            // Incremental hash must equal a full position-sensitive hash of the
            // complete current key, regardless of the preparation-time values.
            U full=Vulkan::PreparedDescriptorKey::Mix(key.words.size());
            for(std::size_t i=0;i<key.words.size();++i)full^=Vulkan::PreparedDescriptorKey::WordHash(key.words[i],i);
            if(key.hash!=full)throw std::runtime_error("incremental descriptor hash is stale");
            buckets[key.hash%256]=true;
            ++cases;
        }
        if(!writes.empty()) {
            vk::BufferView view{};
            for(unsigned reason=0;reason<3;++reason) {
                auto bad=writes;
                if(reason==0)bad.back().pNext=&cases;
                if(reason==1)bad.back().pTexelBufferView=&view;
                if(reason==2){bad.back().pBufferInfo=nullptr;bad.back().pImageInfo=nullptr;}
                key.Build(layout,bad);key.RefreshBuffers();if(key.cacheable)throw std::runtime_error("unsupported descriptor reused");++cases;
            }
        }
    }
    if(std::count(buckets.begin(),buckets.end(),true)<200)throw std::runtime_error("poor descriptor hash bucket coverage");
    {
        vk::DescriptorImageInfo image{vk::Sampler{}, vk::ImageView((VkImageView)(uintptr_t)21), vk::ImageLayout::eGeneral};
        vk::WriteDescriptorSet write{}; write.dstBinding=0; write.descriptorCount=1;
        write.descriptorType=vk::DescriptorType::eSampledImage; write.pImageInfo=&image;
        const auto layout=vk::DescriptorSetLayout((VkDescriptorSetLayout)(uintptr_t)1);
        Vulkan::PreparedDescriptorKey before, snapshot, feedback;
        before.Build(layout,{&write,1});
        image.imageView=vk::ImageView((VkImageView)(uintptr_t)22);
        snapshot.Build(layout,{&write,1});
        image.imageLayout=vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT;
        feedback.Build(layout,{&write,1});
        if(before.words==snapshot.words || snapshot.words==feedback.words)
            throw std::runtime_error("feedback descriptor view/layout reused old key");
        Vulkan::DescriptorSetMemo<unsigned,1> memo;
        memo.Store(before.hash,before.words,7);
        if(memo.Find(snapshot.hash,snapshot.words) || memo.Find(feedback.hash,feedback.words))
            throw std::runtime_error("feedback descriptor reused old set");
    }
    Vulkan::DescriptorSetMemo<unsigned,1> collision;
    const std::array<U,3> one{1,2,3},other{1,2,4};
    collision.Store(7,one,9);
    if(collision.Find(7,one)!=9||collision.Find(7,other)!=0)throw std::runtime_error("hash collision reused wrong descriptors");
    collision.Clear();if(collision.Find(7,one)!=0)throw std::runtime_error("retired descriptor reused");
    std::cout<<"PASS "<<cases<<" descriptor key/commit/reuse cases\n";
}
