// SPDX-License-Identifier: GPL-2.0-or-later
#include "ryty_gpu/native/descriptor_snapshot.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <utility>

// The candidate100/101 ValidatePass predicate, evaluated on the actual owned rows.
static bool OldReplaySafe(const DescriptorSnapshot& snapshot,bool buffers_verified) {
    for(const auto& w:snapshot.Writes()) {
        if(w.pNext || w.pTexelBufferView)return false;
        const bool image=w.descriptorType==vk::DescriptorType::eSampler || w.descriptorType==vk::DescriptorType::eSampledImage;
        const bool buffer=w.descriptorType==vk::DescriptorType::eUniformBuffer || w.descriptorType==vk::DescriptorType::eStorageBuffer;
        if(image ? (w.pBufferInfo || !w.pImageInfo) :
           buffer ? (!buffers_verified || !w.pBufferInfo || w.pImageInfo) : true)return false;
    }
    return true;
}
static void Check(const DescriptorSnapshot& snapshot) {
    for(bool verified:{false,true})assert(snapshot.ParallelReplaySafe(verified)==OldReplaySafe(snapshot,verified));
}
int main() {
    using namespace vk;
    constexpr std::array types{DescriptorType::eUniformBuffer,DescriptorType::eStorageBuffer,
        DescriptorType::eSampler,DescriptorType::eSampledImage,DescriptorType::eCombinedImageSampler,
        DescriptorType::eStorageImage,DescriptorType::eInputAttachment};
    std::array<DescriptorBufferInfo,8> buffers{};
    std::array<DescriptorImageInfo,8> images{};
    DescriptorSnapshot empty;Check(empty);assert(empty.ParallelReplaySafe(false));
    auto factory_empty=DescriptorSnapshot::Make({},buffers,images,0);assert(factory_empty);Check(*factory_empty);
    unsigned cases=0;
    for(unsigned code=0;code<343;++code)for(unsigned count:{1u,2u}) {
        auto remaining=code;
        std::array<WriteDescriptorSet,3> rows{};
        for(unsigned i=0;i<rows.size();++i) {
            auto& row=rows[i];row.descriptorType=types[remaining%types.size()];remaining/=types.size();
            row.dstBinding=i%2;row.dstArrayElement=i;row.descriptorCount=count;
            const bool buffer=row.descriptorType==DescriptorType::eUniformBuffer || row.descriptorType==DescriptorType::eStorageBuffer;
            if(buffer)row.pBufferInfo=buffers.data()+i*2;
            else row.pImageInfo=images.data()+i*2;
        }
        auto made=DescriptorSnapshot::Make(rows,buffers,images,65536);assert(made);Check(*made);
        const auto owned_bytes=made->OwnedBytes();
        std::size_t buffer_count=0;
        for(const auto& row:made->Writes())if(row.pBufferInfo)buffer_count+=row.descriptorCount;
        std::vector<DescriptorBufferInfo> replacement(buffer_count);
        for(auto& info:replacement)info.range=VK_WHOLE_SIZE;
        assert(made->PatchBufferBindings(replacement));Check(*made);
        assert(made->OwnedBytes()==owned_bytes);
        if(!replacement.empty()) {
            assert(!made->PatchBufferBindings(std::span<const DescriptorBufferInfo>{replacement.data(),replacement.size()-1}));
            Check(*made);
        }
        // Public input mutation cannot alter the private factory-owned write metadata.
        for(auto& row:rows) {row.descriptorType=DescriptorType::eUniformTexelBuffer;row.pNext=&rows;row.pBufferInfo=nullptr;row.pImageInfo=nullptr;}
        Check(*made);
        DescriptorSnapshot moved{std::move(*made)};Check(moved);Check(*made);
        assert(moved.OwnedBytes()==owned_bytes);
        DescriptorSnapshot assigned;
        assigned=std::move(moved);Check(assigned);Check(moved);
        DescriptorSnapshot overwritten;
        overwritten=std::move(assigned);Check(overwritten);Check(assigned);
        assert(overwritten.OwnedBytes()==owned_bytes);
        ++cases;
    }
    // Move assignment replaces flags as well as rows, across safe and rejected subsets.
    WriteDescriptorSet row{};row.descriptorCount=1;row.descriptorType=DescriptorType::eStorageImage;row.pImageInfo=images.data();
    auto unsupported=DescriptorSnapshot::Make({&row,1},buffers,images,65536);assert(unsupported);
    assert(!unsupported->ParallelReplaySafe(true));
    row.descriptorType=DescriptorType::eSampledImage;
    auto supported=DescriptorSnapshot::Make({&row,1},buffers,images,65536);assert(supported);
    *unsupported=std::move(*supported);Check(*unsupported);Check(*supported);
    assert(unsupported->ParallelReplaySafe(false));
    row.descriptorType=DescriptorType::eStorageImage;
    auto rejected=DescriptorSnapshot::Make({&row,1},buffers,images,65536);assert(rejected);
    *unsupported=std::move(*rejected);Check(*unsupported);Check(*rejected);
    assert(!unsupported->ParallelReplaySafe(true));
    for(auto type:{DescriptorType::eUniformBuffer,DescriptorType::eStorageImage}) {
        row={};row.descriptorType=type;row.descriptorCount=1;
        if(type==DescriptorType::eUniformBuffer)row.pBufferInfo=buffers.data();
        else row.pImageInfo=images.data();
        auto self=DescriptorSnapshot::Make({&row,1},buffers,images,65536);assert(self);
        auto& alias=*self;
        *self=std::move(alias);Check(*self); // self-move leaves a valid, unspecified state
    }

    row={};row.descriptorType=DescriptorType::eStorageBuffer;row.descriptorCount=1;row.pBufferInfo=buffers.data();
    const auto valid=row;
    for(unsigned malformed=0;malformed<8;++malformed) {
        row=valid;
        if(malformed==0)row.pNext=&row;
        if(malformed==1)row.pTexelBufferView=reinterpret_cast<const BufferView*>(std::uintptr_t{1});
        if(malformed==2)row.descriptorCount=0;
        if(malformed==3)row.pBufferInfo=nullptr;
        if(malformed==4)row.pImageInfo=images.data();
        if(malformed==5)row.pBufferInfo=buffers.data()+buffers.size();
        if(malformed==6)row.descriptorCount=UINT32_MAX;
        if(malformed==7)row.descriptorType=DescriptorType::eUniformTexelBuffer;
        assert(!DescriptorSnapshot::Make({&row,1},buffers,images,65536));
    }
    // The factory payload limit remains the original row/pointer/payload charge.
    struct PointerCharge {bool buffer;std::size_t offset;};
    const auto charge=sizeof(WriteDescriptorSet)+sizeof(PointerCharge)+sizeof(DescriptorBufferInfo);
    row=valid;
    assert(DescriptorSnapshot::Make({&row,1},buffers,images,charge));
    assert(!DescriptorSnapshot::Make({&row,1},buffers,images,charge-1));
    std::printf("PASS %u real snapshot plans: exact parallel subset, verification, owned patching, moves, rejection and payload budgets; sizeof=%zu\n",cases,sizeof(DescriptorSnapshot));
}
