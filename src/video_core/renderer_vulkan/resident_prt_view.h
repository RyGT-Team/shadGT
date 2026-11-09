#pragma once
#include <algorithm>
#include <limits>
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {
// A sampled PRT view can exclude deliberately uncommitted leading mips.
// Rebase only when every selected layout is byte-for-byte equivalent and its
// complete backing validates. No hole is filled, read, or accepted as resident.
template <class ValidBacking>
bool RebaseResidentPrtView(VideoCore::TextureCache::ImageDesc& desc, ValidBacking&& valid) {
    const auto& info=desc.info;
    const auto& view=desc.view_info;
    const u32 first=view.range.base.level, count=view.range.extent.levels;
    if (view.is_storage || info.props.is_depth || info.props.is_volume || !info.props.is_block ||
        info.tile_mode!=AmdGpu::TileMode::ThinThinPrt ||
        info.array_mode!=AmdGpu::GetArrayMode(info.tile_mode) ||
        (info.num_bits!=64 && info.num_bits!=128) || info.type!=AmdGpu::ImageType::Color2D ||
        view.type!=AmdGpu::ImageType::Color2D || info.num_samples!=1 ||
        info.resources.layers!=1 || view.range.base.layer!=0 || view.range.extent.layers!=1 ||
        first==0 || first>=info.resources.levels || first>=info.mips_layout.size() ||
        count==0 || count>info.resources.levels-first || count>info.mips_layout.size()-first) {
        return false;
    }
    const u64 offset=info.mips_layout[first].offset;
    constexpr u64 MaxGpuAddress=0x10000000000;
    if (!info.guest_address || info.guest_address>=MaxGpuAddress ||
        offset>=info.guest_size || offset>MaxGpuAddress-info.guest_address) return false;
    auto reference=info;
    reference.UpdateSize();
    if (reference.guest_size!=info.guest_size ||
        reference.micro_tiled_mips!=info.micro_tiled_mips) return false;
    for (u32 mip=first;mip<first+count;++mip) {
        const auto& actual=info.mips_layout[mip];
        const auto& expected=reference.mips_layout[mip];
        if (actual.offset!=expected.offset || actual.size!=expected.size ||
            actual.pitch!=expected.pitch || actual.height!=expected.height) return false;
    }
    auto candidate=desc;
    candidate.info.guest_address+=offset;
    candidate.info.size.width=std::max(info.size.width>>first,1u);
    candidate.info.size.height=std::max(info.size.height>>first,1u);
    candidate.info.pitch=std::max(info.pitch>>first,1u);
    candidate.info.resources.levels=static_cast<u16>(count);
    // Keep the original physical layout. A later micro-tiled mip must not
    // become macro-tiled merely because it is now the first logical mip.
    candidate.info.micro_tiled_mips=(info.micro_tiled_mips>>first)&((1u<<count)-1);
    const auto& last=info.mips_layout[first+count-1];
    const u64 end=u64(last.offset)+last.size;
    if (end<=offset || end>info.guest_size || end-offset>std::numeric_limits<u32>::max()) return false;
    candidate.info.guest_size=static_cast<u32>(end-offset);
    for (u32 mip=0;mip<candidate.info.mips_layout.size();++mip) {
        if (mip<count) {
            candidate.info.mips_layout[mip]=info.mips_layout[first+mip];
            candidate.info.mips_layout[mip].offset-=static_cast<u32>(offset);
        } else candidate.info.mips_layout[mip]={};
    }
    if (!candidate.info.guest_size || candidate.info.guest_size>info.guest_size-offset ||
        candidate.info.guest_size>MaxGpuAddress-candidate.info.guest_address) return false;
    for (u32 mip=0;mip<count;++mip) {
        const auto& original=info.mips_layout[first+mip];
        const auto& rebased=candidate.info.mips_layout[mip];
        if (original.offset<offset || original.offset-offset!=rebased.offset ||
            original.size!=rebased.size || original.pitch!=rebased.pitch ||
            original.height!=rebased.height ||
            ((info.micro_tiled_mips>>(first+mip))&1)!=((candidate.info.micro_tiled_mips>>mip)&1)) {
            return false;
        }
    }
    if (!valid(candidate.info.guest_address,candidate.info.guest_size)) return false;
    candidate.view_info.range.base.level=0;
    const u32 lod_offset=first*256;
    candidate.view_info.min_lod=view.min_lod>lod_offset?view.min_lod-lod_offset:0;
    desc=std::move(candidate);
    return true;
}
}
