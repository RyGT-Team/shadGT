#pragma once
#include "video_core/renderer_vulkan/prt_recorded_cases.h"

namespace {
bool CheckPrtViewCases(std::string& log) {
    u32 passed=0, accepted=0;
    for (const auto& fixture:PrtRecordedCases) {
        AmdGpu::Image sharp;
        std::memcpy(&sharp,fixture.raw.data(),sizeof(sharp));
        Shader::ImageResource resource{};
        VideoCore::TextureCache::ImageDesc desc{sharp,resource};
        if (desc.info.guest_size!=fixture.bytes) {
            log+=fmt::format("recorded span mismatch {} != {}\n",desc.info.guest_size,fixture.bytes);
            return false;
        }
        const auto original=desc;
        const u32 first=desc.view_info.range.base.level, count=desc.view_info.range.extent.levels;
        const auto start=desc.info.guest_address+desc.info.mips_layout[first].offset;
        const auto& last=desc.info.mips_layout[first+count-1];
        const auto end=desc.info.guest_address+last.offset+last.size;
        const auto backing=[&](u64 address,u64 bytes) {
            return fixture.selected_resident && address>=start && address<end && bytes<=end-address;
        };
        const bool ok=Vulkan::RebaseResidentPrtView(desc,backing);
        if (ok!=fixture.selected_resident) {
            log+=fmt::format("recorded PRT view failed: address={:#x} first={} expected={}\n",
                original.info.guest_address,first,fixture.selected_resident);return false;
        }
        if (ok) {
            if (desc.info.guest_address!=start || desc.info.size.width!=std::max(original.info.size.width>>first,1u) ||
                desc.info.size.height!=std::max(original.info.size.height>>first,1u) ||
                desc.info.resources.levels!=count || desc.view_info.range.base.level!=0 ||
                desc.view_info.range.extent.levels!=count) return false;
            ++accepted;
        }
        ++passed;
    }
    const auto& fixture=PrtRecordedCases[1];
    AmdGpu::Image sharp;std::memcpy(&sharp,fixture.raw.data(),sizeof(sharp));
    Shader::ImageResource resource{};
    const VideoCore::TextureCache::ImageDesc original{sharp,resource};
    const auto valid=[](u64,u64){return true;};
    u32 rejected=0;
    for(u32 mode=0;mode<12;++mode) {
        auto desc=original;
        switch(mode) {
        case 0:desc.view_info.is_storage=true;break;
        case 1:desc.info.props.is_depth=true;break;
        case 2:desc.info.props.is_volume=true;break;
        case 3:desc.info.resources.layers=2;break;
        case 4:desc.view_info.range.base.level=0;break;
        case 5:desc.view_info.range.extent.levels=99;break;
        case 6:desc.info.guest_address=0xffffffffffffffffULL;break;
        case 7:desc.info.mips_layout[2].pitch+=4;break;
        case 8:desc.info.micro_tiled_mips^=1u<<2;break;
        case 9:desc.info.props.is_block=false;break;
        case 10:desc.info.num_samples=4;break;
        case 11:desc.view_info.type=AmdGpu::ImageType::Color2DArray;break;
        }
        if (Vulkan::RebaseResidentPrtView(desc,valid)) return false;
        ++rejected;
    }
    {
        auto desc=original;
        if (Vulkan::RebaseResidentPrtView(desc,[](u64,u64){return false;})) return false;
        if (desc.info.guest_address!=original.info.guest_address ||
            desc.view_info.range.base.level!=original.view_info.range.base.level) return false;
        ++rejected;
    }
    for (u32 lod: {0u,128u,511u,512u,640u,768u,4095u}) {
        auto desc=original;desc.view_info.min_lod=lod;
        const u32 offset=desc.view_info.range.base.level*256;
        if (!Vulkan::RebaseResidentPrtView(desc,valid) ||
            desc.view_info.min_lod!=(lod>offset?lod-offset:0)) return false;
    }
    log+=fmt::format("PRT view: {} actual captured descriptors, {} resident views accepted, {} invalid/unsupported cases rejected, min-LOD normalization passed\n",
        passed,accepted,rejected);
    return true;
}
}
extern "C" __declspec(dllexport) int RytyPrtViewSelfTest(char* report,int capacity) {
    std::string log;
    const bool ok=CheckPrtViewCases(log);
    if (report && capacity>0) {
        const auto n=std::min<std::size_t>(log.size(),capacity-1);
        std::memcpy(report,log.data(),n);report[n]=0;
    }
    return ok?0:1;
}
