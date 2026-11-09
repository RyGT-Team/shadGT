#pragma once
namespace {
bool CheckPrtViewGpu(std::string& log) {
    using namespace Native;
    Shader::ImageResource resource{};
    std::string error;
    auto device=Rhi::CreateStandaloneVulkanDevice(&error);
    if (!device) {log+="device: "+error+"\n";return false;}
    auto fence=device->CreateFence(0);
    u64 tick=0,checked=0;
    // Includes the 512x512 source whose selected first mip is micro-tiled.
    for (u32 case_index:{1u,23u}) {
        if (case_index==23) {
            case_index=0;
            while (case_index<std::size(PrtRecordedCases)) {
                AmdGpu::Image probe;std::memcpy(&probe,PrtRecordedCases[case_index].raw.data(),sizeof(probe));
                if (probe.width==511 && probe.height==511 && PrtRecordedCases[case_index].selected_resident) break;
                ++case_index;
            }
            if (case_index==std::size(PrtRecordedCases)) return false;
        }
        AmdGpu::Image sharp;std::memcpy(&sharp,PrtRecordedCases[case_index].raw.data(),sizeof(sharp));
        VideoCore::TextureCache::ImageDesc desc{sharp,resource};
        const u32 first=desc.view_info.range.base.level;
        const u64 offset=desc.info.mips_layout[first].offset;
        void* allocation=nullptr;
        for(u64 slot=0;slot<16 && !allocation;++slot) {
            allocation=VirtualAlloc(reinterpret_cast<void*>(0x5000000000ULL+slot*0x10000000ULL),
                desc.info.guest_size,MEM_RESERVE,PAGE_NOACCESS);
        }
        if (!allocation) return false;
        struct Release {void* p;~Release(){VirtualFree(p,0,MEM_RELEASE);}} release{allocation};
        desc.info.guest_address=reinterpret_cast<u64>(allocation);
        const auto original=desc;
        if (!VirtualAlloc(static_cast<u8*>(allocation)+offset,desc.info.guest_size-offset,MEM_COMMIT,PAGE_READWRITE)) return false;
        const auto valid=[&](u64 address,u64 bytes) {
            const auto snapshot=SceneResidency::Observe(address,bytes,[](u64 at,SceneResidency::Region& region) {
                MEMORY_BASIC_INFORMATION info{};
                if (!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))) return false;
                const auto begin=reinterpret_cast<u64>(info.BaseAddress);
                region={begin,begin+info.RegionSize,info.State,info.Protect,info.Type};return true;
            },4096);
            const auto coverage=SceneResidency::Cover(snapshot,address,bytes);
            return coverage.complete && coverage.committed==bytes;
        };
        if (valid(original.info.guest_address,original.info.guest_size) ||
            !Vulkan::RebaseResidentPrtView(desc,valid)) return false;
        ImageRegistry producer{*device};
        auto target_info=original.info;target_info.guest_address=0x700000000000ULL+u64(case_index)*0x10000000;
        auto* target=producer.Find(target_info,true,1);
        auto pattern=device->CreateBuffer({.size=target_info.guest_size,.usage=Rhi::BufferUsageTransferSrc,.memory=Rhi::MemoryKind::Upload});
        auto tiled=device->CreateBuffer({.size=target_info.guest_size,.usage=Rhi::BufferUsageStorage|Rhi::BufferUsageTransferDst,.memory=Rhi::MemoryKind::Readback});
        if (!target || !pattern || !tiled || !pattern->Mapped() || !tiled->Mapped()) return false;
        const u32 unit=target_info.num_bits/8;
        std::memset(pattern->Mapped(),0,target_info.guest_size);
        for(u32 mip=0;mip<target_info.resources.levels;++mip) {
            const auto& layout=target_info.mips_layout[mip];
            const u32 width=(std::max(target_info.size.width>>mip,1u)+3)/4;
            const u32 height=(std::max(target_info.size.height>>mip,1u)+3)/4;
            for(u32 y=0;y<height;++y) for(u32 x=0;x<width;++x) for(u32 byte=0;byte<unit;++byte) {
                pattern->Mapped()[layout.offset+(u64(y)*(layout.pitch/4)+x)*unit+byte]=u8(17*mip+3*x+5*y+7*byte);
            }
        }
        auto* list=device->AcquireCommandList();list->Begin();
        producer.RecordUploads(*list);
        list->Barrier(Rhi::ScopeTransfer|Rhi::ScopeCompute,Rhi::ScopeTransfer);
        list->CopyBufferToImage(pattern.get(),target->image.get(),MipCopies(target_info));
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeTransfer);
        if (!producer.RecordDownload(*list,*target,*tiled)) return false;
        list->Barrier(Rhi::ScopeTransfer|Rhi::ScopeCompute,Rhi::ScopeHost);list->End();
        Rhi::CommandList* lists[]={list};
        device->GraphicsQueue().Submit(lists,{}, {fence.get(),++tick});
        if (!fence->Wait(tick,5'000'000'000ULL)) return false;
        tiled->Invalidate();
        std::memcpy(reinterpret_cast<void*>(desc.info.guest_address),tiled->Mapped()+offset,desc.info.guest_size);
        ImageRegistry consumer{*device};
        consumer.SetBackingValidator(valid);
        consumer.SetUploadSource([&](const ImageEntry& entry,Rhi::CommandList&) {
            ImageRegistry::UploadSource source;
            if (!valid(entry.info.guest_address,entry.info.guest_size)) return source;
            auto host=device->CreateBuffer({.size=entry.info.guest_size,
                .usage=Rhi::BufferUsageStorage|Rhi::BufferUsageTransferSrc,.memory=Rhi::MemoryKind::Upload});
            if (!host || !host->Mapped()) return source;
            std::memcpy(host->Mapped(),reinterpret_cast<const void*>(entry.info.guest_address),entry.info.guest_size);
            source.buffer=host.get();source.kind=4;source.keep.push_back(std::move(host));return source;
        });
        auto* texture=consumer.Find(desc.info,false,2);
        if (!texture) return false;
        const auto format=Rhi::FormatFromVk(u32(desc.info.pixel_format));
        auto original_view=device->CreateImageView(target->image.get(),{.format=format,.base_mip=first,.mips=desc.info.resources.levels});
        auto selected_view=device->CreateImageView(texture->image.get(),{.format=format,.base_mip=0,.mips=desc.info.resources.levels});
        if (!original_view || !selected_view) return false;
        std::array<std::unique_ptr<Rhi::Buffer>,2> reads;
        for(auto& read:reads) {
            read=device->CreateBuffer({.size=desc.info.guest_size,.usage=Rhi::BufferUsageTransferDst,.memory=Rhi::MemoryKind::Readback});
            if (!read || !read->Mapped()) return false;
        }
        list=device->AcquireCommandList();list->Begin();
        for(u32 phase=0;phase<2;++phase) {
            if (phase) {
                for(u32 byte=0;byte<unit;++byte)reinterpret_cast<u8*>(desc.info.guest_address)[byte]^=0x5a;
                consumer.CpuWrite(desc.info.guest_address,unit,4);
                consumer.QueueUploadForUse(*texture);
            }
            if (consumer.RecordUploads(*list)!=1) return false;
            list->Barrier(Rhi::ScopeTransfer|Rhi::ScopeCompute,Rhi::ScopeTransfer);
            list->CopyImageToBuffer(texture->image.get(),reads[phase].get(),MipCopies(desc.info));
            list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeCompute|Rhi::ScopeTransfer);
        }
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeHost);list->End();lists[0]=list;
        device->GraphicsQueue().Submit(lists,{}, {fence.get(),++tick});
        if (!fence->Wait(tick,5'000'000'000ULL)) return false;
        for(u32 phase=0;phase<2;++phase) {
            reads[phase]->Invalidate();
            for(u32 mip=0;mip<desc.info.resources.levels;++mip) {
                const auto& layout=desc.info.mips_layout[mip];
                const auto& original_layout=original.info.mips_layout[first+mip];
                const u32 width=(std::max(desc.info.size.width>>mip,1u)+3)/4;
                const u32 height=(std::max(desc.info.size.height>>mip,1u)+3)/4;
                for(u32 y=0;y<height;++y) for(u32 x=0;x<width;++x) for(u32 byte=0;byte<unit;++byte) {
                    u8 expected=pattern->Mapped()[original_layout.offset+(u64(y)*(original_layout.pitch/4)+x)*unit+byte];
                    if (phase && mip==0 && x==0 && y==0) expected^=0x5a;
                    const u8 actual=reads[phase]->Mapped()[layout.offset+(u64(y)*(layout.pitch/4)+x)*unit+byte];
                    if (actual!=expected) {
                        log+=fmt::format("PRT GPU mismatch case={} phase={} mip={} block={}/{} byte={} got={} wanted={}\n",
                            case_index,phase,mip,x,y,byte,actual,expected);return false;
                    }
                    ++checked;
                }
            }
        }
        log+=fmt::format("PRT GPU {}x{} base mip {}: reserved leading mips, selected mip layouts, two queued mutation snapshots passed\n",
            original.info.size.width,original.info.size.height,first);
    }
    log+=fmt::format("PRT GPU selected-mip bytes verified: {}\n",checked);
    return true;
}
}
extern "C" __declspec(dllexport) int RytyPrtViewGpuSelfTest(char* report,int capacity) {
    std::string log;
    const bool ok=CheckPrtViewGpu(log);
    if (report && capacity>0) {const auto n=std::min<std::size_t>(log.size(),capacity-1);std::memcpy(report,log.data(),n);report[n]=0;}
    return ok?0:1;
}
