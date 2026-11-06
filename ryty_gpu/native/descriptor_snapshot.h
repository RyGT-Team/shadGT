#pragma once
#include "video_core/renderer_vulkan/vk_common.h"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Isolated worker-packet prototype: copies and rebases descriptors, never retains
// DrawBindings pointers. It does not pin GPU objects or perform descriptor updates.
class DescriptorSnapshot {
    struct OrdinaryRow {std::size_t source_offset;std::uint32_t set;bool buffer;};
    std::vector<vk::WriteDescriptorSet> writes_;
    std::vector<vk::DescriptorBufferInfo> buffers_;
    std::vector<vk::DescriptorImageInfo> images_;
    std::vector<OrdinaryRow> ordinary_rows_;
    bool ordinary_ready_=false;
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
    std::size_t OwnedBytes() const noexcept {return sizeof(*this)+writes_.capacity()*sizeof(vk::WriteDescriptorSet)+buffers_.capacity()*sizeof(vk::DescriptorBufferInfo)+images_.capacity()*sizeof(vk::DescriptorImageInfo)+ordinary_rows_.capacity()*sizeof(OrdinaryRow);}
    template<class Groups>
    static std::optional<DescriptorSnapshot> MakeOrdinary(const Groups& groups,std::size_t push,
        std::span<const vk::DescriptorBufferInfo> buffers,std::span<const vk::DescriptorImageInfo> images,std::size_t budget) {
        if(push>groups.size())return std::nullopt;
        std::vector<vk::WriteDescriptorSet> rows;std::vector<OrdinaryRow> mapping;
        for(std::size_t set=0;set<groups.size();++set)if(set!=push)for(const auto& w:groups[set]) {
            if(rows.size()>=budget/(sizeof(vk::WriteDescriptorSet)+sizeof(OrdinaryRow)))return std::nullopt;
            const bool buffer=w.pBufferInfo!=nullptr;
            if(buffer ? !BufferSliceValid(w.pBufferInfo,w.descriptorCount,buffers) : !ImageSliceValid(w.pImageInfo,w.descriptorCount,images))return std::nullopt;
            const auto offset=buffer?std::size_t(w.pBufferInfo-buffers.data()):std::size_t(w.pImageInfo-images.data());
            rows.push_back(w);mapping.push_back({offset,static_cast<std::uint32_t>(set),buffer});
        }
        if(mapping.capacity()>budget/sizeof(OrdinaryRow))return std::nullopt;
        const auto map_bytes=mapping.capacity()*sizeof(OrdinaryRow);
        auto result=Make(rows,buffers,images,budget-map_bytes);if(!result)return std::nullopt;
        result->ordinary_rows_=std::move(mapping);
        result->ordinary_ready_=true;
        if(result->OwnedBytes()>budget)return std::nullopt;
        return result;
    }
    template<class Groups,class Sets>
    bool PatchOrdinary(const Groups& groups,std::size_t push,const Sets& misses,
        std::span<const vk::DescriptorBufferInfo> buffers,std::span<const vk::DescriptorImageInfo> images) noexcept {
        if(!ordinary_ready_ || writes_.size()!=ordinary_rows_.size() || misses.size()<groups.size() || push>groups.size())return false;
        std::size_t n=0;
        // Validate the complete live mapping before modifying any payload.
        for(std::size_t set=0;set<groups.size();++set)if(set!=push)for(const auto& row:groups[set]) {
            if(n>=ordinary_rows_.size())return false;
            const auto& map=ordinary_rows_[n];const auto& old=writes_[n++];
            if(map.set!=set || map.set>=misses.size() || old.dstBinding!=row.dstBinding || old.dstArrayElement!=row.dstArrayElement ||
               old.descriptorCount!=row.descriptorCount || old.descriptorType!=row.descriptorType || row.pNext || row.pTexelBufferView)return false;
            if(map.buffer) {if(row.pImageInfo || !BufferSliceValid(row.pBufferInfo,row.descriptorCount,buffers) || std::size_t(row.pBufferInfo-buffers.data())!=map.source_offset)return false;}
            else {if(row.pBufferInfo || !ImageSliceValid(row.pImageInfo,row.descriptorCount,images) || std::size_t(row.pImageInfo-images.data())!=map.source_offset)return false;}
        }
        if(n!=ordinary_rows_.size())return false;
        std::size_t output=0;
        for(std::size_t i=0;i<n;++i) {
            const auto& map=ordinary_rows_[i];auto row=writes_[i];
            if(!misses[map.set])continue;
            if(map.buffer)std::copy_n(buffers.data()+map.source_offset,row.descriptorCount,const_cast<vk::DescriptorBufferInfo*>(row.pBufferInfo));
            else std::copy_n(images.data()+map.source_offset,row.descriptorCount,const_cast<vk::DescriptorImageInfo*>(row.pImageInfo));
            row.dstSet=misses[map.set];writes_[output++]=row;
        }
        writes_.resize(output);ordinary_ready_=false;return true;
    }
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

// Only the recording worker, or its serial fallback after worker completion,
// may initialize these fresh sets. The producer never accesses this state
// after publishing the draw.
struct OwnedDescriptorUpdates {
    DescriptorSnapshot snapshot;
    bool initialized=false;
    explicit OwnedDescriptorUpdates(DescriptorSnapshot&& s):snapshot(std::move(s)) {}
    template<class Update> void Ensure(Update&& update) {
        if(!initialized) { update(snapshot.Writes()); initialized=true; }
    }
};
