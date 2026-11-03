#pragma once
#include "video_core/renderer_vulkan/vk_common.h"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Isolated worker-packet prototype: copies and rebases descriptors, never retains
// DrawBindings pointers. It does not pin GPU objects or perform descriptor updates.
class DescriptorSnapshot {
    std::vector<vk::WriteDescriptorSet> writes_;
    std::vector<vk::DescriptorBufferInfo> buffers_;
    std::vector<vk::DescriptorImageInfo> images_;
    bool parallel_subset_supported_=true;
    bool has_buffer_descriptors_=false;
    template<class T> static bool Inside(const T* ptr,std::size_t count,std::span<const T> source) {
        if(!ptr || count==0 || count>source.size()) return false;
        const auto base=reinterpret_cast<std::uintptr_t>(source.data()),at=reinterpret_cast<std::uintptr_t>(ptr);
        if(at<base) return false;
        const auto offset=at-base;
        return offset%sizeof(T)==0 && offset/sizeof(T)<=source.size()-count;
    }
public:
    DescriptorSnapshot()=default;
    DescriptorSnapshot(const DescriptorSnapshot&)=delete;
    DescriptorSnapshot& operator=(const DescriptorSnapshot&)=delete;
    DescriptorSnapshot(DescriptorSnapshot&&)=default;
    DescriptorSnapshot& operator=(DescriptorSnapshot&&)=default;
    static bool ImageSliceValid(const vk::DescriptorImageInfo* ptr,std::size_t count,std::span<const vk::DescriptorImageInfo> source) {return Inside(ptr,count,source);}
    static bool BufferSliceValid(const vk::DescriptorBufferInfo* ptr,std::size_t count,std::span<const vk::DescriptorBufferInfo> source) {return Inside(ptr,count,source);}
    std::span<const vk::WriteDescriptorSet> Writes() const {return writes_;}
    // Make validates the private rows and rebases their pointers into owned storage.
    // Patching changes only buffer values. Empty also covers moved-from snapshots.
    bool ParallelReplaySafe(bool buffers_verified) const noexcept {
        return writes_.empty() || (parallel_subset_supported_ && (!has_buffer_descriptors_ || buffers_verified));
    }
    // Called only by the sole mutable owner before publishing the snapshot.
    bool PatchBufferBindings(std::span<const vk::DescriptorBufferInfo> source) noexcept {
        if (source.size() != buffers_.size()) return false;
        std::copy(source.begin(), source.end(), buffers_.begin());
        return true;
    }
    std::size_t OwnedBytes() const noexcept {return sizeof(*this)+writes_.capacity()*sizeof(vk::WriteDescriptorSet)+buffers_.capacity()*sizeof(vk::DescriptorBufferInfo)+images_.capacity()*sizeof(vk::DescriptorImageInfo);}
    static std::optional<DescriptorSnapshot> Make(std::span<const vk::WriteDescriptorSet> writes,
        std::span<const vk::DescriptorBufferInfo> buffers,std::span<const vk::DescriptorImageInfo> images, std::size_t max_payload_bytes) {
        DescriptorSnapshot out;
        // Keep the original Pointer charge even though no temporary vector is
        // needed; the accepted payload budget must remain identical.
        struct Pointer {bool buffer;std::size_t offset;};
        std::size_t used=0,buffer_count=0,image_count=0;
        const auto charge=[&](std::size_t count,std::size_t size) {
            if(count>(max_payload_bytes-used)/size) return false;
            used+=count*size;return true;
        };
        if(!charge(writes.size(),sizeof(vk::WriteDescriptorSet)) || !charge(writes.size(),sizeof(Pointer))) return std::nullopt;
        // Validate and charge every repeated source slice before copying anything.
        for(const auto& w:writes) {
            if(w.pNext || w.pTexelBufferView || !w.descriptorCount) return std::nullopt;
            const bool is_buffer=w.descriptorType==vk::DescriptorType::eUniformBuffer || w.descriptorType==vk::DescriptorType::eStorageBuffer;
            const bool is_image=w.descriptorType==vk::DescriptorType::eSampler || w.descriptorType==vk::DescriptorType::eCombinedImageSampler ||
                w.descriptorType==vk::DescriptorType::eSampledImage || w.descriptorType==vk::DescriptorType::eStorageImage || w.descriptorType==vk::DescriptorType::eInputAttachment;
            if(is_buffer) {if(w.pImageInfo || !Inside(w.pBufferInfo,w.descriptorCount,buffers) || !charge(w.descriptorCount,sizeof(vk::DescriptorBufferInfo)) || w.descriptorCount>SIZE_MAX-buffer_count)return std::nullopt;buffer_count+=w.descriptorCount;}
            else if(is_image) {if(w.pBufferInfo || !Inside(w.pImageInfo,w.descriptorCount,images) || !charge(w.descriptorCount,sizeof(vk::DescriptorImageInfo)) || w.descriptorCount>SIZE_MAX-image_count)return std::nullopt;image_count+=w.descriptorCount;}
            else return std::nullopt;
        }
        out.writes_.reserve(writes.size());out.buffers_.reserve(buffer_count);out.images_.reserve(image_count);
        for(const auto& w:writes) {
            if(w.pNext || w.pTexelBufferView || !w.descriptorCount) return std::nullopt;
            const bool is_buffer = w.descriptorType==vk::DescriptorType::eUniformBuffer || w.descriptorType==vk::DescriptorType::eStorageBuffer;
            const bool is_image = w.descriptorType==vk::DescriptorType::eSampler || w.descriptorType==vk::DescriptorType::eCombinedImageSampler ||
                w.descriptorType==vk::DescriptorType::eSampledImage || w.descriptorType==vk::DescriptorType::eStorageImage || w.descriptorType==vk::DescriptorType::eInputAttachment;
            if(is_buffer) {
                if(w.pImageInfo || !Inside(w.pBufferInfo,w.descriptorCount,buffers)) return std::nullopt;
                out.buffers_.insert(out.buffers_.end(),w.pBufferInfo,w.pBufferInfo+w.descriptorCount);
            } else if(is_image) {
                if(w.pBufferInfo || !Inside(w.pImageInfo,w.descriptorCount,images)) return std::nullopt;
                out.images_.insert(out.images_.end(),w.pImageInfo,w.pImageInfo+w.descriptorCount);
            } else return std::nullopt;
            out.writes_.push_back(w);
        }
        std::size_t buffer_offset=0,image_offset=0;
        for(auto& w:out.writes_) {
            const bool is_buffer=w.descriptorType==vk::DescriptorType::eUniformBuffer || w.descriptorType==vk::DescriptorType::eStorageBuffer;
            out.parallel_subset_supported_ &= is_buffer || w.descriptorType==vk::DescriptorType::eSampler || w.descriptorType==vk::DescriptorType::eSampledImage;
            out.has_buffer_descriptors_ |= is_buffer;
            if(is_buffer){w.pBufferInfo=out.buffers_.data()+buffer_offset;buffer_offset+=w.descriptorCount;}
            else {w.pImageInfo=out.images_.data()+image_offset;image_offset+=w.descriptorCount;}
        }
        return out;
    }
};
