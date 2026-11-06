// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <vulkan/vulkan.hpp>
#include "common/assert.h"
#include "ryty_gpu/native/rhi/rhi.h"
#include "video_core/renderer_vulkan/native_async_fill_spv.h"

namespace Vulkan {
// The shader consumes only captured CPU constants and writes private memory.
// Graphics copies that result at the original fill position, waiting on its fence.
// Slots (including descriptors and buffers) survive through that graphics join.
class NativeAsyncFill {
public:
    static constexpr u64 MaxBytes = 16 * 1024 * 1024;
    static constexpr u64 ByteBudget = 64 * 1024 * 1024;
    static constexpr std::size_t SlotCount = 8;
    struct Dispatch { vk::Buffer buffer; u64 value; };

    static std::unique_ptr<NativeAsyncFill> Create(Native::Rhi::Device& owner,
                                                  vk::Device device,
                                                  Native::Rhi::Fence& graphics_fence) {
        if (!owner.HasAsyncCompute()) return nullptr;
        auto result=std::unique_ptr<NativeAsyncFill>(new NativeAsyncFill(owner,device,graphics_fence));
        if (!result->Init()) return nullptr;
        return result;
    }
    ~NativeAsyncFill() {
        // Normal ownership closes the graphics join before destruction. This also
        // handles an abandoned, compute-submitted slot that never got a copyback.
        if (last_value) ASSERT_MSG(fence->Wait(last_value,5'000'000'000ULL),
                                   "async fill did not finish before destruction");
    }
    Native::Rhi::Fence& Fence() const { return *fence; }
    u64 PendingWait() const { return pending_wait; }
    u64 Submitted() const { return last_value; }
    u64 ActiveBytes() const { return active_bytes; }
    void ConsumeWait(u64 value) { if (pending_wait==value) pending_wait=0; }
    void MarkGraphicsTick(u64 tick) {
        ASSERT(tick!=0);
        for(auto& slot:slots) if(slot.buffer && !slot.graphics_tick) slot.graphics_tick=tick;
    }
    void Collect() {
        const auto completed=graphics.Completed();
        const auto compute_completed=fence->Completed();
        for(auto& slot:slots) if(slot.buffer && slot.graphics_tick && slot.graphics_tick<=completed &&
                                slot.compute_value<=compute_completed) {
            active_bytes-=slot.buffer->Size();
            slot.buffer.reset(); slot.graphics_tick=slot.compute_value=0;
        }
    }
    std::optional<Dispatch> TryDispatch(u64 bytes,u32 even_word,u32 odd_word) {
        if(!bytes || bytes%4 || bytes>MaxBytes || last_value==std::numeric_limits<u64>::max())return {};
        Collect();
        if(bytes>ByteBudget-active_bytes)return {};
        auto found=std::find_if(slots.begin(),slots.end(),[](const auto& slot){return !slot.buffer;});
        if(found==slots.end())return {};
        auto output=owner.CreateBuffer({.size=bytes,
            .usage=Native::Rhi::BufferUsageStorage|Native::Rhi::BufferUsageTransferSrc,
            .memory=Native::Rhi::MemoryKind::DeviceLocal,.name="private async fill output"});
        if(!output)return {};
        const vk::Buffer buffer{reinterpret_cast<VkBuffer>(output->NativeHandle())};
        const vk::DescriptorBufferInfo binding{buffer,0,bytes};
        const vk::WriteDescriptorSet write{.dstSet=found->set,.dstBinding=0,
            .descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&binding};
        device.updateDescriptorSets(write,{});
        auto* list=owner.AcquireComputeCommandList(Native::Rhi::CommandListLevel::Primary);
        if(!list)return {};
        list->Begin();
        const vk::CommandBuffer cmd{reinterpret_cast<VkCommandBuffer>(list->NativeHandle())};
        const std::array<u32,3> parameters{u32(bytes/4),even_word,odd_word};
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,*layout,0,found->set,{});
        cmd.pushConstants(*layout,vk::ShaderStageFlagBits::eCompute,0,sizeof(parameters),parameters.data());
        cmd.dispatch(u32((bytes/4+127)/128),1,1);
        list->End();
        const u64 value=last_value+1;
        Native::Rhi::CommandList* lists[]{list};
        // No GPU input exists: no graphics-producer wait is needed for this kernel.
        owner.ComputeQueue().Submit(lists,{}, {fence.get(),value});
        found->buffer=std::move(output); found->graphics_tick=0;found->compute_value=value;
        active_bytes+=bytes; last_value=pending_wait=value;
        return Dispatch{buffer,value};
    }
private:
    struct Slot {vk::DescriptorSet set{};std::unique_ptr<Native::Rhi::Buffer> buffer;u64 graphics_tick{},compute_value{};};
    NativeAsyncFill(Native::Rhi::Device& owner,vk::Device device,Native::Rhi::Fence& graphics)
        :owner(owner),device(device),graphics(graphics) {}
    bool Init() {
        fence=owner.CreateFence(0);if(!fence)return false;
        const vk::DescriptorSetLayoutBinding binding{0,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute};
        auto [sr,s]=device.createDescriptorSetLayoutUnique({.bindingCount=1,.pBindings=&binding});
        if(sr!=vk::Result::eSuccess)return false;set_layout=std::move(s);
        const vk::DescriptorSetLayout raw=*set_layout;
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute,0,12};
        auto [lr,l]=device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&raw,
            .pushConstantRangeCount=1,.pPushConstantRanges=&push});
        if(lr!=vk::Result::eSuccess)return false;layout=std::move(l);
        auto [mr,m]=device.createShaderModuleUnique({.codeSize=sizeof(kNativeAsyncFillSpv),.pCode=kNativeAsyncFillSpv});
        if(mr!=vk::Result::eSuccess)return false;
        auto [pr,p]=device.createComputePipelineUnique({},vk::ComputePipelineCreateInfo{
            .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=*m,.pName="main"},.layout=*layout});
        if(pr!=vk::Result::eSuccess)return false;pipeline=std::move(p);
        const vk::DescriptorPoolSize count{vk::DescriptorType::eStorageBuffer,u32(SlotCount)};
        auto [dr,d]=device.createDescriptorPoolUnique({.maxSets=u32(SlotCount),.poolSizeCount=1,.pPoolSizes=&count});
        if(dr!=vk::Result::eSuccess)return false;pool=std::move(d);
        std::array<vk::DescriptorSetLayout,SlotCount> layouts;layouts.fill(*set_layout);
        auto [ar,a]=device.allocateDescriptorSets({.descriptorPool=*pool,.descriptorSetCount=u32(SlotCount),.pSetLayouts=layouts.data()});
        if(ar!=vk::Result::eSuccess)return false;
        for(std::size_t i=0;i<slots.size();++i)slots[i].set=a[i];
        return true;
    }
    Native::Rhi::Device& owner;
    vk::Device device;
    Native::Rhi::Fence& graphics;
    std::unique_ptr<Native::Rhi::Fence> fence;
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    vk::UniqueDescriptorPool pool;
    std::array<Slot,SlotCount> slots;
    u64 last_value{},pending_wait{},active_bytes{};
};
}
