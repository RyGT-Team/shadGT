#include "ryty_gpu/native/cpu_texture_staging.h"
#include "ryty_gpu/native/region_init_gaps.h"
#include "ryty_gpu/native/local_tiled_source.h"
#include "video_core/renderer_vulkan/async_mirror_buffer.h"
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// NAT-021: self test of the native image registry, outside the game. For a micro-tiled, a
// macro-tiled and a display-tiled layout with mips, a pattern is put in a render target, written
// out in the guest's tiled layout (RecordDownload), handed back as a texture in "guest memory"
// (RecordUploads untiles it) and compared texel by texel.

#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>
#include <fmt/format.h>

#include "ryty_gpu/native/image_registry.h"
#include "ryty_gpu/native/rhi/rhi_vulkan.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/renderer_vulkan/upload_chunk_cache.h"
#include <deque>

namespace {

using namespace Native;

VideoCore::ImageInfo MakeInfo(AmdGpu::TileMode tile_mode, u32 width, u32 height, u32 mips,
                              VAddr address) {
    VideoCore::ImageInfo info{};
    info.props = {};
    info.props.is_tiled = tile_mode != AmdGpu::TileMode::DisplayLinearAligned;
    info.props.is_pow2 = std::has_single_bit(width) && std::has_single_bit(height);
    info.tile_mode = tile_mode;
    info.array_mode = AmdGpu::GetArrayMode(tile_mode);
    info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    info.num_samples = 1;
    info.num_bits = 32;
    info.type = AmdGpu::ImageType::Color2D;
    info.size = {width, height, 1};
    info.pitch = width;
    info.resources = {static_cast<u16>(mips), 1};
    info.guest_address = address;
    info.UpdateSize();
    return info;
}

u32 Texel(u32 x, u32 y, u32 mip) {
    return x | (y << 10) | (mip << 20) | 0xA0000000u;
}

bool RunCase(Rhi::Device& device, ImageRegistry& registry, Rhi::Fence& fence, u64& tick,
             AmdGpu::TileMode tile_mode, std::string& log,
             u32 kWidth = 256, u32 kHeight = 256) {
    registry.SetUploadSource({}); // Do not carry a previous case's CPU-only fixture into targets.
    constexpr u32 kMips = 4;
    const auto name = AmdGpu::NameOf(tile_mode);
    // 1. A render target filled with the pattern from a linear buffer.
    const VAddr target_address = 0x7000'0000'0000ULL + (u64(tile_mode) << 24);
    const auto target_info = MakeInfo(tile_mode, kWidth, kHeight, kMips,
                                     target_address + (kWidth > 256 ? 0x100000000ULL : 0));
    const u64 size = target_info.guest_size;
    ImageEntry* target = registry.Find(target_info, true, 1);
    auto pattern = device.CreateBuffer({.size = size,
                                        .usage = Rhi::BufferUsageTransferSrc,
                                        .memory = Rhi::MemoryKind::Upload});
    auto tiled = device.CreateBuffer({.size = size,
                                      .usage = Rhi::BufferUsageStorage | Rhi::BufferUsageTransferDst,
                                      .memory = Rhi::MemoryKind::Readback});
    if (!target || !pattern || !tiled) {
        log += fmt::format("{}: setup failed\n", name);
        return false;
    }
    std::memset(pattern->Mapped(), 0, size);
    for (u32 m = 0; m < kMips; ++m) {
        const auto& mip = target_info.mips_layout[m];
        const u32 width = std::max(kWidth >> m, 1u), height = std::max(kHeight >> m, 1u);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const u32 value = Texel(x, y, m);
                std::memcpy(pattern->Mapped() + mip.offset + (u64(y) * mip.pitch + x) * 4, &value, 4);
            }
        }
    }
    Rhi::CommandList* list = device.AcquireCommandList();
    list->Begin();
    registry.RecordUploads(*list);
    list->Barrier(Rhi::ScopeTransfer | Rhi::ScopeCompute, Rhi::ScopeTransfer);
    list->CopyBufferToImage(pattern.get(), target->image.get(), MipCopies(target_info));
    list->Barrier(Rhi::ScopeTransfer, Rhi::ScopeTransfer);
    if (!registry.RecordDownload(*list, *target, *tiled)) {
        log += fmt::format("{}: download not recorded\n", name);
        return false;
    }
    list->Barrier(Rhi::ScopeTransfer | Rhi::ScopeCompute, Rhi::ScopeHost);
    list->End();
    Rhi::CommandList* lists[] = {list};
    device.GraphicsQueue().Submit(lists, {}, {&fence, ++tick});
    if (!fence.Wait(tick, 5'000'000'000ULL)) {
        log += fmt::format("{}: download did not finish\n", name);
        return false;
    }
    // 2. The tiled bytes as guest memory, found as a texture and untiled by the upload.
    std::vector<u8> guest(size + 4096);
    u8* guest_memory = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(guest.data()) + 4095) & ~uintptr_t{4095});
    std::memcpy(guest_memory, tiled->Mapped(), size);
    const auto texture_info =
        MakeInfo(tile_mode, kWidth, kHeight, kMips, reinterpret_cast<VAddr>(guest_memory));
    if (kWidth >= 2048) {
        registry.SetUploadSource([&device,&registry](const ImageEntry& entry, Rhi::CommandList& upload_list) {
            if (!entry.from_cpu) return ImageRegistry::UploadSource{};
            const auto bytes = entry.info.guest_size;
            auto host = device.CreateBuffer({.size = bytes,
                .usage = Rhi::BufferUsageStorage | Rhi::BufferUsageTransferSrc,
                .memory = Rhi::MemoryKind::Upload});
            ImageRegistry::UploadSource source;
            if (!host || !host->Mapped()) return source;
            std::memcpy(host->Mapped(),reinterpret_cast<const void*>(entry.info.guest_address),bytes);
            source.buffer = host.get(); source.kind = 4;
            source.keep.push_back(std::move(host));
            return Native::LocalTiledSource(registry,upload_list,std::move(source),bytes,
                                            entry.info.props.is_tiled,0x10de);
        });
    }
    ImageEntry* texture = registry.Find(texture_info, false, 2);
    auto readback = device.CreateBuffer({.size = size,
                                         .usage = Rhi::BufferUsageTransferDst,
                                         .memory = Rhi::MemoryKind::Readback});
    if (!texture || !readback) {
        log += fmt::format("{}: texture setup failed\n", name);
        return false;
    }
    std::memset(readback->Mapped(), 0, size);
    list = device.AcquireCommandList();
    list->Begin();
    const u32 uploads = registry.RecordUploads(*list);
    u32 expected_first = Texel(0,0,0);
    if (texture_info.guest_size >= (4ull << 20)) {
        // Refresh before any GPU invalidation: the latest CPU snapshot remains eligible.
        if (texture->gpu_backing_written.load()) return false;
        registry.CpuWrite(texture_info.guest_address,4096,4);
        registry.QueueUploadForUse(*texture);
        if (registry.RecordUploads(*list) != 1 || texture->gpu_backing_written.load()) return false;
        // Unmap is reported before the old guest mapping is released. Discarding
        // it must not eagerly copy the old bytes or forget the later refresh.
        for (u32 unmap=0;unmap<32;++unmap) {
            registry.Invalidate(texture_info.guest_address,4096,ImageRegistry::Stale::Unmap);
            if (!texture->needs_upload.load() || !texture->force_upload.load() ||
                texture->gpu_backing_written.load() || registry.RecordUploads(*list)!=0) {
                log += "discarded texture uploaded eagerly or lost its remap state\n"; return false;
            }
        }
        expected_first=0xA5DECAFEu;
        std::memcpy(guest_memory,&expected_first,4);
        registry.QueueUploadForUse(*texture);
        registry.QueueUploadForUse(*texture);
        if (registry.RecordUploads(*list)!=1 || registry.RecordUploads(*list)!=0 ||
            texture->force_upload.load() || texture->gpu_backing_written.load()) return false;
        // Validate the remapped bytes on the GPU before later mutation cases overwrite them.
        auto remap_read=device.CreateBuffer({.size=4,.usage=Rhi::BufferUsageTransferDst,
            .memory=Rhi::MemoryKind::Readback});
        if (!remap_read) return false;
        const Rhi::BufferImageCopy pixel{.extent={1,1,1}};
        list->Barrier(Rhi::ScopeTransfer | Rhi::ScopeCompute,Rhi::ScopeTransfer);
        list->CopyImageToBuffer(texture->image.get(),remap_read.get(),{&pixel,1});
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeHost);list->End();
        lists[0]=list;device.GraphicsQueue().Submit(lists,{}, {&fence,++tick});
        if (!fence.Wait(tick,5'000'000'000ULL)) return false;
        remap_read->Invalidate();u32 remapped_pixel;std::memcpy(&remapped_pixel,remap_read->Mapped(),4);
        if (remapped_pixel!=expected_first) {log += "remapped texture returned old pixels\n";return false;}
        list=device.AcquireCommandList();list->Begin();
        log += "32 unmappings deferred; remapped latest bytes refreshed once on use: passed\n";
        for (u32 write = 0; write < 32; ++write) {
            registry.GpuWrote(texture_info.guest_address,texture_info.guest_size,nullptr,true,write+1);
            if (!texture->needs_upload.load() || registry.RecordUploads(*list) != 0) {
                log += "deferred texture refreshed before use\n"; return false;
            }
        }
        registry.QueueUploadForUse(*texture);
        registry.QueueUploadForUse(*texture);
        if (registry.RecordUploads(*list) != 1 || registry.RecordUploads(*list) != 0) {
            log += "deferred texture use was not coalesced\n"; return false;
        }
        log += "32 streaming writes deferred until use, duplicate admissions coalesced: passed\n";
        u32 first_tiled;
        std::memcpy(&first_tiled,guest_memory,4);
        if (first_tiled != expected_first) { log += "unexpected first tiled texel location\n"; return false; }
        for (u32 write = 0; write < 32; ++write) {
            registry.CpuWrite(texture_info.guest_address,4096,4);
            expected_first = 0xA5000000u | (write+1);
            std::memcpy(guest_memory,&expected_first,4);
            if (!texture->needs_upload.load() || texture->force_upload.load() ||
                registry.RecordUploads(*list) != 0) {
                log += "CPU streaming texture refreshed before use\n"; return false;
            }
        }
        registry.QueueUploadForUse(*texture);
        // A later write before recording must be part of the single consumed snapshot.
        registry.CpuWrite(texture_info.guest_address,4096,4);
        expected_first = 0xA5FEDCBAu;
        std::memcpy(guest_memory,&expected_first,4);
        registry.QueueUploadForUse(*texture);
        if (registry.RecordUploads(*list) != 1 || registry.RecordUploads(*list) != 0) {
            log += "CPU streaming use was not coalesced\n"; return false;
        }
        log += "33 CPU mutations deferred until use, latest queued bytes selected once: passed\n";
        if (!texture->gpu_backing_written.load() || texture->force_upload.load()) {
            log += "GPU ownership was lost after a later CPU update\n"; return false;
        }
        // The per-upload force flag was consumed, but partially GPU-owned memory
        // must remain ineligible for CPU-only snapshots on all future refreshes.
        if ((texture_info.guest_size <= (64ull << 20) &&
             !PreferCpuTextureStaging(texture_info.guest_size,true,true,true,false,false,false,false,true)) ||
            PreferCpuTextureStaging(texture_info.guest_size,true,true,
                !texture->gpu_backing_written.load(),false,false,false,false,true)) return false;
        log += "CPU refresh staging and sticky GPU ownership: passed\n";
    }
    const auto* saved_params = texture->download_params.get();
    if (!saved_params) {
        log += fmt::format("{}: missing cached upload parameters\n", name);
        return false;
    }
    // Several uploads in the same list must retain their immutable parameter buffer
    // and still produce the expected pixels after the final dispatch/copy barriers.
    for (u32 repeat = 0; repeat < 8; ++repeat) {
        list->Barrier(Rhi::ScopeTransfer | Rhi::ScopeCompute, Rhi::ScopeTransfer | Rhi::ScopeCompute);
        registry.RequestUpload(*texture);
        if (registry.RecordUploads(*list) != 1 || texture->download_params.get() != saved_params) {
            log += fmt::format("{}: repeated upload parameter reuse failed\n", name);
            return false;
        }
    }
    // Upload a different picture through the same scratch, then read both images.
    // A missing WAR barrier can overwrite the first picture's bytes before its copy.
    std::vector<u8> other_guest(size + 4096);
    auto* other_memory = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(other_guest.data()) + 4095) & ~uintptr_t{4095});
    for (u64 i = 0; i < size; ++i) other_memory[i] = guest_memory[i] ^ 0x5a;
    const auto other_info = MakeInfo(tile_mode, kWidth, kHeight, kMips, reinterpret_cast<VAddr>(other_memory));
    ImageEntry* other_texture = registry.Find(other_info, false, 2);
    auto other_readback = device.CreateBuffer({.size = size,
        .usage = Rhi::BufferUsageTransferDst, .memory = Rhi::MemoryKind::Readback});
    if (!other_texture || !other_readback || registry.RecordUploads(*list) != 1) {
        log += fmt::format("{}: second scratch image setup failed\n", name);
        return false;
    }
    list->CopyImageToBuffer(other_texture->image.get(), other_readback.get(), MipCopies(other_info));
    list->CopyImageToBuffer(texture->image.get(), readback.get(), MipCopies(texture_info));
    list->Barrier(Rhi::ScopeTransfer, Rhi::ScopeHost);
    list->End();
    lists[0] = list;
    device.GraphicsQueue().Submit(lists, {}, {&fence, ++tick});
    if (!fence.Wait(tick, 5'000'000'000ULL)) {
        log += fmt::format("{}: upload did not finish\n", name);
        return false;
    }
    u64 wrong = 0, total = 0;
    for (u32 m = 0; m < kMips; ++m) {
        const auto& mip = texture_info.mips_layout[m];
        const u32 width = std::max(kWidth >> m, 1u), height = std::max(kHeight >> m, 1u);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                u32 value;
                std::memcpy(&value, readback->Mapped() + mip.offset + (u64(y) * mip.pitch + x) * 4, 4);
                const u32 expected = (m == 0 && x == 0 && y == 0) ? expected_first : Texel(x,y,m);
                wrong += value != expected;
                u32 other_value;
                std::memcpy(&other_value, other_readback->Mapped() + mip.offset + (u64(y) * mip.pitch + x) * 4, 4);
                wrong += other_value != (expected ^ 0x5a5a5a5au);
                total += 2;
            }
        }
    }
    // The tiled bytes must differ from the linear ones (otherwise nothing was tiled).
    const bool changed = std::memcmp(tiled->Mapped(), pattern->Mapped(), size) != 0;
    log += fmt::format("{}: {} uploaded, guest size {} bytes, tiled layout {}, {} of {} texels {}\n",
                       name, uploads, size, changed ? "differs from linear" : "SAME AS LINEAR",
                       total - wrong, total, wrong == 0 ? "correct" : "WRONG");
    return wrong == 0 && uploads == 1 && changed;
}

bool RunResolveFormatCase(ImageRegistry& registry, std::string& log) {
    auto scalar_info = MakeInfo(AmdGpu::TileMode::Thin1DThin, 480, 270, 1, 0x7100'0000'0000ULL);
    scalar_info.pixel_format = vk::Format::eR32Sfloat;
    auto* scalar = registry.Find(scalar_info, true, 1);
    auto color_info = scalar_info;
    color_info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    auto* aliased = registry.Find(color_info, true, 2);
    auto* color = registry.Find(color_info, true, 3, true, true);
    auto* repeated = registry.Find(color_info, true, 4, true, true);
    auto sampled_info = color_info;
    sampled_info.size.width = 430;
    sampled_info.UpdateSize();
    auto* sampled = registry.Find(sampled_info, false, 5);
    bool linked = false;
    if (sampled) {
        for (const auto& link : sampled->sources) {
            linked |= link.source == color;
        }
    }
    auto ms_scalar_info = scalar_info;
    ms_scalar_info.guest_address += 0x1000000;
    ms_scalar_info.num_samples = 2;
    ms_scalar_info.UpdateSize();
    auto* ms_scalar = registry.Find(ms_scalar_info, true, 6);
    auto ms_color_info = ms_scalar_info;
    ms_color_info.pixel_format = color_info.pixel_format;
    auto* ms_color = registry.Find(ms_color_info, true, 7);
    const bool ok =
        scalar && aliased == scalar && color && color != scalar && repeated == color &&
        color->image->Desc().format == Rhi::FormatFromVk(u32(color_info.pixel_format)) && linked &&
        ms_scalar && ms_color && ms_scalar != ms_color &&
        ms_color->image->Desc().format == color->image->Desc().format;
    log += fmt::format("resolve format isolation and cropped texture linking: {}\n",
                       ok ? "passed" : "FAILED");
    return ok;
}

bool RunUploadChunkCacheCase(Rhi::Device& device, Rhi::Fence& fence, u64& tick, std::string& log) {
    struct Chunk { std::unique_ptr<Rhi::Buffer> buffer; u64 retire_tick; };
    std::vector<Chunk> free;
    std::deque<Chunk> retired;
    constexpr u64 bytes = 0x1556800;
    std::array<std::unique_ptr<Rhi::Buffer>, 2> reads;
    for (u32 i=0; i<2; ++i) {
        auto buffer = device.CreateBuffer({
            .size=Vulkan::UploadChunkCapacity(bytes,8ull<<20),
            .usage=Rhi::BufferUsageTransferSrc, .memory=Rhi::MemoryKind::Upload});
        reads[i] = device.CreateBuffer({.size=bytes, .usage=Rhi::BufferUsageTransferDst,
                                       .memory=Rhi::MemoryKind::Readback});
        if (!buffer || !buffer->Mapped() || !reads[i] || !reads[i]->Mapped()) return false;
        std::memset(buffer->Mapped(), i ? 0xa7 : 0x35, bytes);
        auto* list = device.AcquireCommandList();
        list->Begin();
        list->CopyBuffer(buffer.get(),0,reads[i].get(),0,bytes);
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeHost);
        list->End();
        Rhi::CommandList* lists[]{list};
        device.GraphicsQueue().Submit(lists,{}, {&fence,++tick});
        retired.push_back({std::move(buffer),tick});
        // No CPU wait between submissions; neither upload may be recycled early.
        Vulkan::CacheCompletedUploadChunks(free,retired,0);
        if (!free.empty()) return false;
    }
    if (!fence.Wait(tick,5'000'000'000ULL)) return false;
    Vulkan::CacheCompletedUploadChunks(free,retired,fence.Completed());
    bool ok = retired.empty() && free.size()==2;
    for (u32 i=0;i<2;++i) {
        const auto* data=reads[i]->Mapped();
        const u8 expected=i ? 0xa7 : 0x35;
        for (u64 at=0;at<bytes;++at) if (data[at]!=expected) {ok=false;break;}
    }
    log += fmt::format("large upload chunk cache: {} bytes, two in-flight snapshots, {}\n",
                       2*bytes,ok ? "passed" : "FAILED");
    return ok;
}

bool RunLargeGatherScratchCase(Rhi::Device& device, Rhi::Fence& fence, u64& tick,
                               std::string& log) {
    constexpr u64 bytes = 147ull << 20;
    ImageRegistry registry{device};
    std::array<std::unique_ptr<Rhi::Buffer>, 2> reads;
    auto small_read = device.CreateBuffer({.size=4ull<<20,
        .usage=Rhi::BufferUsageTransferDst, .memory=Rhi::MemoryKind::Readback});
    if (!small_read) return false;
    const Rhi::Buffer* shared = nullptr;
    for (u32 i=0; i<2; ++i) {
        reads[i] = device.CreateBuffer({.size=bytes, .usage=Rhi::BufferUsageTransferDst,
                                       .memory=Rhi::MemoryKind::Readback});
        if (!reads[i]) return false;
        auto* list = device.AcquireCommandList(); list->Begin();
        if (i==0) {
            auto* small = registry.AcquireUploadGatherScratch(*list,4ull<<20);
            if (!small) return false;
            list->FillBuffer(small,0,4ull<<20,0xccccccccu);
            list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeTransfer);
            list->CopyBuffer(small,0,small_read.get(),0,4ull<<20);
            // Grow before this command list is even submitted; the old allocation
            // must remain alive until its queued copy completes.
        }
        auto* scratch = registry.AcquireUploadGatherScratch(*list, bytes);
        if (!scratch || (shared && scratch != shared)) return false;
        shared = scratch;
        list->FillBuffer(scratch,0,bytes,i ? 0xa7a7a7a7u : 0x35353535u);
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeTransfer);
        list->CopyBuffer(scratch,0,reads[i].get(),0,bytes);
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeHost);
        list->End(); Rhi::CommandList* lists[]{list};
        device.GraphicsQueue().Submit(lists,{}, {&fence,++tick});
        // The second write must not overtake the first snapshot: no CPU wait here.
    }
    if (!fence.Wait(tick,10'000'000'000ULL)) return false;
    bool ok = true;
    small_read->Invalidate();
    for (u64 at=0;at<(4ull<<20);++at) if(small_read->Mapped()[at]!=0xcc) {ok=false;break;}
    for (u32 i=0;i<2;++i) {
        reads[i]->Invalidate();
        const u8 expected=i ? 0xa7 : 0x35;
        for (u64 at=0;at<bytes;++at) {
            if (reads[i]->Mapped()[at]!=expected) {ok=false;break;}
        }
    }
    log += fmt::format("shared large gather: {} bytes in two queued snapshots plus growth-before-submit, {}; retained {} bytes\n",
                       2*bytes,ok ? "passed" : "FAILED",shared->Size());
    return ok;
}

bool RunRegionMergeOrderCase(Rhi::Device& device, Rhi::Fence& fence, u64& tick,
                            std::string& log) {
    constexpr u64 bytes = 4 * 1024 * 1024;
    auto base = device.CreateBuffer({.size=bytes, .usage=Rhi::BufferUsageTransferSrc,
                                    .memory=Rhi::MemoryKind::Upload});
    auto retained = device.CreateBuffer({.size=bytes,
        .usage=Rhi::BufferUsageTransferSrc | Rhi::BufferUsageTransferDst,
        .memory=Rhi::MemoryKind::DeviceLocal});
    auto patch = device.CreateBuffer({.size=bytes, .usage=Rhi::BufferUsageTransferSrc,
                                     .memory=Rhi::MemoryKind::Upload});
    auto merged = device.CreateBuffer({.size=bytes,
        .usage=Rhi::BufferUsageTransferSrc | Rhi::BufferUsageTransferDst,
        .memory=Rhi::MemoryKind::DeviceLocal});
    auto read = device.CreateBuffer({.size=bytes, .usage=Rhi::BufferUsageTransferDst,
                                    .memory=Rhi::MemoryKind::Readback});
    if (!base || !retained || !patch || !merged || !read || !base->Mapped() ||
        !patch->Mapped() || !read->Mapped()) return false;
    struct Span { u64 dst, src, size; };
    // Descriptor tables, a scalar loop bound, counts, and a larger GPU-owned range.
    const std::array<Span,4> small_spans{{{4096,0,48}, {8192,4096,16},
                                   {2*1024*1024+64,8192,676},
                                   {3*1024*1024,16384,16384}}};
    std::vector<u8> expected(bytes);
    bool ok = true;
    for (u32 pass=0; pass<16 && ok; ++pass) {
        for (u64 i=0;i<bytes;++i) base->Mapped()[i]=u8(i*13+pass*7);
        for (u64 i=0;i<bytes;++i) patch->Mapped()[i]=u8(i*29+pass*31+0x57);
        std::vector<Span> spans(small_spans.begin(),small_spans.end());
        switch (pass % 5) {
        case 1: spans={{0,0,bytes}}; break;
        case 2: spans={{0,0,bytes/4}}; break;
        case 3: spans={{3*bytes/4,0,bytes/4}}; break;
        case 4: spans.clear(); break;
        }
        std::vector<RegionInitRange> covered;
        for (auto span : spans) covered.push_back({span.dst,span.dst+span.size});
        const auto gaps=RegionInitGaps(0,bytes,covered);
        std::memcpy(expected.data(),base->Mapped(),bytes);
        for (const auto& span:spans)
            std::memcpy(expected.data()+span.dst,patch->Mapped()+span.src,span.size);
        auto* list=device.AcquireCommandList();
        list->Begin();
        list->CopyBuffer(patch.get(),0,retained.get(),0,bytes);
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeTransfer);
        for (const auto gap : gaps) {
            // Simulate an import traversal that copied a prefix, then failed.
            // The complete sparse fallback must replace that partial result.
            if (pass % 3 == 0) {
                list->CopyBuffer(patch.get(),0,merged.get(),gap.begin,
                                 std::min<u64>(4096,gap.end-gap.begin));
                list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeTransfer);
            }
            list->CopyBuffer(base.get(),gap.begin,merged.get(),gap.begin,gap.end-gap.begin);
        }
        // Same transfer-write dependency used by MakeRegion before its overlays.
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeTransfer);
        for (const auto& span:spans)
            list->CopyBuffer(retained.get(),span.src,merged.get(),span.dst,span.size);
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeTransfer);
        // A pending CPU page snapshot still refreshes the selected bytes after merge.
        if (!spans.empty()) {
            list->CopyBuffer(base.get(),0,merged.get(),spans.front().dst,4);
            std::memcpy(expected.data()+spans.front().dst,base->Mapped(),4);
            list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeTransfer);
        }
        list->CopyBuffer(merged.get(),0,read.get(),0,bytes);
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeHost);
        list->End();
        Rhi::CommandList* lists[]{list};
        device.GraphicsQueue().Submit(lists,{}, {&fence,++tick});
        ok=fence.Wait(tick,5'000'000'000ULL) &&
           std::memcmp(read->Mapped(),expected.data(),bytes)==0;
    }
    log+=fmt::format("region gap initialization: 16 mixed CPU/GPU/dirty-patch patterns, {} bytes, {}\n",
                     bytes*16,ok ? "passed" : "FAILED");
    return ok;
}

bool RunUnmapTargetLifetimeCase(Rhi::Device& device, std::string& log) {
    ImageRegistry registry{device};
    auto info = MakeInfo(AmdGpu::TileMode::DisplayLinearAligned, 16, 16, 1,
                         0x720000000000ull);
    ImageEntry* target = registry.Find(info, true, 1);
    if (!target || !target->image) return false;
    bool ok = registry.Find(info, false, 2) == target;
    registry.CpuWrite(info.guest_address, info.guest_size, info.guest_size);
    ok = ok && !target->reused.load() && registry.Find(info, false, 3) == target;
    const u64 generation = registry.Generation();
    const std::pair<VAddr, VAddr> range{info.guest_address, info.guest_address + info.guest_size};
    registry.Invalidate(info.guest_address + 1, 1, ImageRegistry::Stale::Unmap);
    ok = ok && target->reused.load() && !registry.Untouched(generation, {&range, 1});
    ok = ok && target->image && registry.Count() == 1;
    ImageEntry* texture = registry.Find(info, false, 4);
    ok = ok && texture && texture != target && texture->from_cpu && registry.Count() == 2;
    ok = ok && registry.Find(info, false, 5) == texture;
    u32 retired = 0;
    ok = registry.Evict(10, [&](ImageEntry& e) { if (&e == target) ++retired; },
                        [&](ImageEntry& e) { return &e == target; }) == 1 &&
         retired == 0 && registry.Count() == 1 && ok;
    ok = registry.Evict(10, [&](ImageEntry& e) { if (&e == target) ++retired; }) == 1 &&
         retired == 1 && registry.Count() == 0 && ok;
    log += fmt::format("unmapped target lifetime and alias: {}\n", ok ? "passed" : "FAILED");
    return ok;
}

bool RunEvictionBudgetCase(Rhi::Device& device, std::string& log) {
    ImageRegistry registry{device};
    constexpr VAddr base = 0x710000000000ull;
    constexpr u32 count = 192;
    for (u32 i = 0; i < count; ++i) {
        auto info = MakeInfo(AmdGpu::TileMode::DisplayLinearAligned, 16, 16, 1,
                             base + u64{i} * 65536);
        if (!registry.Find(info, false, i >= 160 ? 1000 : 1)) return false;
    }
    const auto keep_index = [](u32 i) { return i >= 160 || i % 17 == 0 || i == 7; };
    u32 expected = 0;
    for (u32 i = 0; i < count; ++i) expected += keep_index(i);
    std::unordered_set<VAddr> retired;
    bool ok = true;
    const auto on_evict = [&](ImageEntry& entry) {
        const u32 i = u32((entry.info.guest_address - base) / 65536);
        ok = ok && !keep_index(i) && retired.insert(entry.info.guest_address).second;
    };
    const auto keep = [&](ImageEntry& entry) {
        const u32 i = u32((entry.info.guest_address - base) / 65536);
        // Simulate a lookup racing the candidate scan: the final last_use check
        // must retain this image even though the keep callback says false.
        if (i == 7) entry.last_use.store(1000);
        return i % 17 == 0;
    };
    ok = registry.Evict(100, on_evict, keep, 0) == 0 && registry.Count() == count && ok;
    for (u32 pass = 0; pass < 128 && registry.Count() > expected; ++pass) {
        const u32 removed = registry.Evict(100, on_evict, keep, 16);
        ok = removed <= 16 && registry.Count() == count - retired.size() && ok;
    }
    ok = registry.Count() == expected && retired.size() == count - expected && ok;
    for (u32 i = 0; i < count; ++i) {
        auto* found = registry.FindExisting(base + u64{i} * 65536,
                                            [](const ImageEntry&) { return true; });
        ok = (found != nullptr) == keep_index(i) && ok;
    }
    u32 final = 0;
    ok = registry.Evict(2000, [&](ImageEntry&) { ++final; }) == expected &&
         final == expected && registry.Count() == 0 && ok;
    // A hot shard must sustain the full retirement budget each frame. Visiting
    // only a few shards per frame would silently reduce throughput by 16x here.
    ImageRegistry concentrated{device};
    u32 made = 0;
    for (VAddr address = base; made < 64; address += 65536) {
        if (((address >> 12) * 0x9E3779B97F4A7C15ULL >> 58) != 0) continue;
        if (!concentrated.Find(MakeInfo(AmdGpu::TileMode::DisplayLinearAligned,
                                        16, 16, 1, address), false, 1)) return false;
        ++made;
    }
    u32 hot_removed = 0;
    for (u32 pass = 0; pass < 4; ++pass) {
        const u32 removed = concentrated.Evict(100, [](ImageEntry&) {}, {}, 16);
        ok = removed == 16 && ok;
        hot_removed += removed;
    }
    ok = hot_removed == 64 && concentrated.Count() == 0 && ok;
    log += fmt::format("incremental image retirement: {} images plus 64 in one shard, 16-per-pass cap, kept/recent/raced-use preservation, full drain: {}\n",
                       count, ok ? "passed" : "FAILED");
    return ok;
}

bool RunAsyncMirrorCase(Rhi::Device& device, Rhi::Fence& fence, u64& tick,
                        std::string& log) {
    Vulkan::AsyncMirrorBuffer<Rhi::Buffer> worker([&](auto request) {
        return device.CreateBuffer({.size = request.end - request.begin,
            .usage = Rhi::BufferUsageVertex | Rhi::BufferUsageIndex |
                     Rhi::BufferUsageStorage | Rhi::BufferUsageTransferSrc |
                     Rhi::BufferUsageTransferDst,
            .memory = Rhi::MemoryKind::DeviceLocal});
    });
    u64 verified = 0;
    for (const u64 bytes : {4096ull, 1ull << 20, 8ull << 20}) {
        for (u64 generation = 1; generation <= 2; ++generation) {
            auto source = device.CreateBuffer({.size = bytes,
                .usage = Rhi::BufferUsageTransferSrc, .memory = Rhi::MemoryKind::Upload});
            auto readback = device.CreateBuffer({.size = bytes,
                .usage = Rhi::BufferUsageTransferDst, .memory = Rhi::MemoryKind::Readback});
            if (!source || !readback) return false;
            auto* words = reinterpret_cast<u32*>(source->Mapped());
            for (u64 i = 0; i < bytes / 4; ++i) words[i] = u32(i * 2654435761u) ^ u32(generation);
            std::unique_ptr<Rhi::Buffer> mirror;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!(mirror = worker.TryAcquire({4096, 4096 + bytes, generation}))) {
                if (std::chrono::steady_clock::now() > deadline) {
                    log += "async mirror: allocation timed out\n"; return false;
                }
                std::this_thread::yield();
            }
            auto* list = device.AcquireCommandList(); list->Begin();
            list->Barrier(Rhi::ScopeHost, Rhi::ScopeTransfer);
            list->CopyBuffer(source.get(), 0, mirror.get(), 0, bytes);
            list->Barrier(Rhi::ScopeTransfer, Rhi::ScopeTransfer);
            list->CopyBuffer(mirror.get(), 0, readback.get(), 0, bytes);
            list->Barrier(Rhi::ScopeTransfer, Rhi::ScopeHost); list->End();
            Rhi::CommandList* lists[] = {list};
            device.GraphicsQueue().Submit(lists, {}, {&fence, ++tick});
            if (!fence.Wait(tick, 5'000'000'000ULL) ||
                std::memcmp(source->Mapped(), readback->Mapped(), bytes) != 0) {
                log += "async mirror: ordered GPU copy mismatch\n"; return false;
            }
            verified += bytes;
        }
    }
    // Two blank allocations requested for A are filled from different B inputs
    // in one GPU submission. Reuse must never imply that A's bytes were copied.
    constexpr u64 bytes=4096;
    using Worker=Vulkan::AsyncMirrorBuffer<Rhi::Buffer>;
    std::array<std::unique_ptr<Rhi::Buffer>,2> sources,mirrors,readbacks;
    for(u64 i=0;i<2;++i) {
        sources[i]=device.CreateBuffer({.size=bytes,.usage=Rhi::BufferUsageTransferSrc,.memory=Rhi::MemoryKind::Upload});
        readbacks[i]=device.CreateBuffer({.size=bytes,.usage=Rhi::BufferUsageTransferDst,.memory=Rhi::MemoryKind::Readback});
        if(!sources[i] || !readbacks[i] || !sources[i]->Mapped() || !readbacks[i]->Mapped())return false;
        std::memset(sources[i]->Mapped(),0xa5,bytes);
        const u64 a=0x10000+i*0x40000,b=a+0x20000;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        u32 starts=0;
        while(!worker.TryPrepare({a,a+bytes,3},[&]{++starts;return true;})) {
            if(std::chrono::steady_clock::now()>deadline)return false;
            std::this_thread::yield();
        }
        if(starts!=1)return false;
        Worker::Observation observed{};
        while(!(mirrors[i]=worker.TryAcquire({b,b+bytes,3},&observed))) {
            if(std::chrono::steady_clock::now()>deadline)return false;
            std::this_thread::yield();
        }
        if(observed!=Worker::Observation::Reused)return false;
        // Mutate B after A's allocation is ready: copy the latest B bytes.
        for(u64 j=0;j<bytes;++j)sources[i]->Mapped()[j]=u8((j*17+i*91+31)&255);
    }
    auto* list=device.AcquireCommandList();list->Begin();list->Barrier(Rhi::ScopeHost,Rhi::ScopeTransfer);
    for(u32 i=0;i<2;++i)list->CopyBuffer(sources[i].get(),0,mirrors[i].get(),0,bytes);
    list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeTransfer);
    for(u32 i=0;i<2;++i)list->CopyBuffer(mirrors[i].get(),0,readbacks[i].get(),0,bytes);
    list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeHost);list->End();
    Rhi::CommandList* lists[]={list};device.GraphicsQueue().Submit(lists,{}, {&fence,++tick});
    if(!fence.Wait(tick,5'000'000'000ULL))return false;
    for(u32 i=0;i<2;++i) {readbacks[i]->Invalidate();if(std::memcmp(sources[i]->Mapped(),readbacks[i]->Mapped(),bytes))return false;}
    log += fmt::format("async mirror: six exact allocations and two queue-only cross-address allocations, ordered GPU copies, {} bytes verified: passed\n", verified+2*bytes);
    return true;
}


// Distinct CPU and GPU fixtures make a wrong ownership decision visible in GPU bytes.
bool RunCpuRefreshOwnershipCase(Rhi::Device& device, Rhi::Fence& fence, u64& tick,
                               std::string& log) {
    constexpr u64 bytes = 2048ull * 1024 * 4;
    std::vector<u8> guest(bytes,0x12);
    auto info = MakeInfo(AmdGpu::TileMode::Thin2DThin,2048,1024,1,
                         reinterpret_cast<VAddr>(guest.data()));
    info.props.is_tiled = false;
    info.UpdateSize();
    if (info.guest_size != bytes) return false;
    ImageRegistry registry{device};
    auto gpu = device.CreateBuffer({.size=bytes,
        .usage=Rhi::BufferUsageStorage | Rhi::BufferUsageTransferDst,
        .memory=Rhi::MemoryKind::DeviceLocal});
    auto seed = device.CreateBuffer({.size=bytes,.usage=Rhi::BufferUsageTransferSrc,
        .memory=Rhi::MemoryKind::Upload});
    if (!gpu || !seed) return false;
    std::memset(seed->Mapped(),0x78,bytes);
    auto* list=device.AcquireCommandList();list->Begin();
    list->CopyBuffer(seed.get(),0,gpu.get(),0,bytes);
    list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeTransfer | Rhi::ScopeCompute);
    u32 cpu_selected=0,gpu_selected=0;
    bool write_during_snapshot=true;
    registry.SetUploadSource([&](const ImageEntry& entry,Rhi::CommandList&) {
        ImageRegistry::UploadSource source;
        if (PreferCpuTextureStaging(bytes,true,entry.from_cpu,
                !entry.gpu_backing_written.load(),false,false,false,false,true)) {
            auto frozen=device.CreateBuffer({.size=bytes,
                .usage=Rhi::BufferUsageStorage | Rhi::BufferUsageTransferSrc,
                .memory=Rhi::MemoryKind::Upload});
            if (!frozen || !frozen->Mapped()) return source;
            std::memcpy(frozen->Mapped(),guest.data(),bytes);
            if (write_during_snapshot) {
                write_during_snapshot=false;
                registry.CpuWrite(entry.info.guest_address,bytes,bytes);
                std::memset(guest.data(),0xa6,bytes);
            }
            source.buffer=frozen.get();source.kind=4;source.keep.push_back(std::move(frozen));
            ++cpu_selected;
        } else {
            source.buffer=gpu.get();source.kind=1;++gpu_selected;
        }
        return source;
    });
    auto* texture=registry.Find(info,false,1);
    if (!texture) return false;
    // Multiple bindings/admissions reference this image before the same drain.
    for (u32 i=0;i<18;++i) registry.QueueUploadForUse(*texture);
    bool upload_count_ok=true;
    std::array<std::unique_ptr<Rhi::Buffer>,4> readbacks;
    for (u32 phase=0;phase<4;++phase) {
        if (phase==1) {
            // No second write notification: the write during phase zero must
            // already be retained and admitted by this next actual use alone.
            registry.QueueUploadForUse(*texture);
        } else if (phase==2) {
            registry.GpuWrote(info.guest_address,4,nullptr,true,0);
            registry.QueueUploadForUse(*texture);
        } else if (phase==3) {
            if (texture->force_upload.load()) return false;
            registry.CpuWrite(info.guest_address,4,4);
            std::memset(guest.data(),0x56,4);
            registry.QueueUploadForUse(*texture);
        }
        if (registry.RecordUploads(*list)!=1) upload_count_ok=false;
        if (phase==0 && (!texture->needs_upload.load() || registry.RecordUploads(*list)!=0)) upload_count_ok=false;
        readbacks[phase]=device.CreateBuffer({.size=bytes,.usage=Rhi::BufferUsageTransferDst,
            .memory=Rhi::MemoryKind::Readback});
        if (!readbacks[phase]) return false;
        list->Barrier(Rhi::ScopeTransfer | Rhi::ScopeCompute,Rhi::ScopeTransfer);
        list->CopyImageToBuffer(texture->image.get(),readbacks[phase].get(),MipCopies(info));
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeTransfer | Rhi::ScopeCompute);
    }
    // These changes happen before submit; neither frozen CPU snapshot may see them.
    std::memset(guest.data(),0xab,bytes);
    list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeHost);list->End();
    Rhi::CommandList* lists[]={list};
    device.GraphicsQueue().Submit(lists,{}, {&fence,++tick});
    if (!fence.Wait(tick,5'000'000'000ULL)) return false;
    const std::array<u8,4> expected{0x12,0xa6,0x78,0x78};
    for (u32 phase=0;phase<4;++phase) {
        readbacks[phase]->Invalidate();
        for (u64 i=0;i<bytes;++i) if (readbacks[phase]->Mapped()[i]!=expected[phase]) {
            log += fmt::format("CPU/GPU ownership phase {} mismatch at {}\n",phase,i);
            return false;
        }
    }
    const bool ok=upload_count_ok && cpu_selected==2 && gpu_selected==2 && texture->gpu_backing_written.load();
    if (ok) log += "upload duplicate race: dirty write during snapshot survives for the next use: passed\n";
    log += fmt::format("CPU refresh snapshots/partial GPU write/later CPU write: {} CPU, {} GPU, 32 MiB verified; {}\n",
        cpu_selected,gpu_selected,ok?"passed":"FAILED");
    return ok;
}

bool RunImageSelfTest(std::string& log) {
    std::string error;
    auto device = Rhi::CreateStandaloneVulkanDevice(&error);
    if (!device) {
        log += "no device: " + error + "\n";
        return false;
    }
    auto fence = device->CreateFence(0);
    u64 tick = 0;
    bool ok = true;
    ok = RunAsyncMirrorCase(*device,*fence,tick,log) && ok;
    ok = RunEvictionBudgetCase(*device, log) && ok;
    ok = RunUnmapTargetLifetimeCase(*device, log) && ok;
    ok = RunUploadChunkCacheCase(*device,*fence,tick,log) && ok;
    ok = RunLargeGatherScratchCase(*device,*fence,tick,log) && ok;
    ok = RunRegionMergeOrderCase(*device,*fence,tick,log) && ok;
    ok = RunCpuRefreshOwnershipCase(*device,*fence,tick,log) && ok;
    {
        ImageRegistry registry{*device};
        ok = RunResolveFormatCase(registry, log) && ok;
        for (const auto mode : {AmdGpu::TileMode::Thin1DThin, AmdGpu::TileMode::Thin2DThin,
                                AmdGpu::TileMode::Display2DThin}) {
            ok = RunCase(*device, registry, *fence, tick, mode, log) && ok;
        }
        const auto& reuse = registry.GetUploadStats();
        // Exercise the actual large CPU staging + untiling/copy path with two
        // distinct textures, repeated uploads and shared scratch barriers.
        ok = RunCase(*device, registry, *fence, tick, AmdGpu::TileMode::Thin2DThin,
                     log, 2048, 2048) && ok;
        ok = RunCase(*device, registry, *fence, tick, AmdGpu::TileMode::Thin2DThin,
                     log, 6144, 4608) && ok;
        const bool scratch_ok = reuse.scratch_created.load() < reuse.scratch_reused.load() &&
                                reuse.scratch_transient.load() == 0;
        ok = scratch_ok && ok;
        log += fmt::format("shared upload scratch: {} created, {} uses, {} transient; {}\n",
            reuse.scratch_created.load(), reuse.scratch_reused.load(),
            reuse.scratch_transient.load(), scratch_ok ? "passed" : "FAILED");
        const auto& stats = registry.GetStats();
        log += fmt::format("registry: {} lookups, {} textures, {} render targets, {} uploads\n",
                           stats.lookups.load(), stats.created_textures.load(),
                           stats.created_targets.load(), stats.uploads.load());
    }
    std::ignore = fence->Wait(tick, 5'000'000'000ULL);
    device->Collect();
    return ok;
}

} // namespace

extern "C" __declspec(dllexport) int RytyNativeImageSelfTest(char* report, int report_size) {
    std::string log;
    const bool ok = RunImageSelfTest(log);
    log += ok ? "NAT-021 image self test passed\n" : "NAT-021 image self test FAILED\n";
    if (report && report_size > 0) {
        const size_t n = std::min<size_t>(log.size(), static_cast<size_t>(report_size) - 1);
        std::memcpy(report, log.data(), n);
        report[n] = '\0';
    }
    return ok ? 0 : 1;
}

extern "C" __declspec(dllexport) int RytyNativeLocalUntileBenchmark(char* report, int report_size) {
    using namespace Native;
    using namespace std::chrono;
    std::string error;
    auto device = Rhi::CreateStandaloneVulkanDevice(&error);
    if (!device) return 1;
    auto fence = device->CreateFence(0);
    ImageRegistry registry{*device};
    auto info = MakeInfo(AmdGpu::TileMode::Thin2DThin,4096,4096,13,0x7000'0000'0000ULL);
    info.pixel_format = vk::Format::eBc7UnormBlock;
    info.num_bits = 128; info.props.is_block = true; info.UpdateSize();
    const u64 size = info.guest_size;
    constexpr u64 offset = 256;
    auto host = device->CreateBuffer({.size = size + offset,
        .usage = Rhi::BufferUsageStorage | Rhi::BufferUsageTransferSrc,
        .memory = Rhi::MemoryKind::Upload});
    auto read = device->CreateBuffer({.size = size,
        .usage = Rhi::BufferUsageTransferDst, .memory = Rhi::MemoryKind::Readback});
    if (!host || !read) return 2;
    for (u64 i = 0; i < size + offset; ++i) host->Mapped()[i] = u8((i*37) ^ (i>>9));
    bool stage = false;
    registry.SetUploadSource([&](const ImageEntry&, Rhi::CommandList& list) {
        ImageRegistry::UploadSource source;
        source.buffer = host.get(); source.offset = offset; source.kind = 2;
        if (stage) source = Native::LocalTiledSource(registry,list,std::move(source),size,true,0x10de);
        return source;
    });
    auto* texture = registry.Find(info,false,1);
    if (!texture) return 3;
    std::vector<u8> reference(size);
    std::array<std::vector<double>,2> timings;
    u64 tick = 0;
    for (u32 iteration=0; iteration<44; ++iteration) {
        // Warm both paths, then interleave/reverse AB order across trial pairs.
        stage = iteration%4==1 || iteration%4==2;
        registry.RequestUpload(*texture);
        std::memset(read->Mapped(),0,size);
        auto* list = device->AcquireCommandList();list->Begin();
        if (registry.RecordUploads(*list)!=1) return 4;
        list->End();
        Rhi::CommandList* lists[]={list};
        const auto start = steady_clock::now();
        device->GraphicsQueue().Submit(lists,{}, {fence.get(),++tick});
        if (!fence->Wait(tick,5'000'000'000ULL)) return 5;
        const auto elapsed = duration<double,std::milli>(steady_clock::now()-start).count();
        // Verify through a separate submission; device-to-host verification
        // must not dominate the upload-only comparison being measured.
        list = device->AcquireCommandList(); list->Begin();
        list->Barrier(Rhi::ScopeTransfer | Rhi::ScopeCompute,Rhi::ScopeTransfer);
        list->CopyImageToBuffer(texture->image.get(),read.get(),MipCopies(info));
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeHost);list->End();
        lists[0] = list;
        device->GraphicsQueue().Submit(lists,{}, {fence.get(),++tick});
        if (!fence->Wait(tick,5'000'000'000ULL)) return 7;
        read->Invalidate();
        if (iteration==0) std::memcpy(reference.data(),read->Mapped(),size);
        else if (std::memcmp(reference.data(),read->Mapped(),size)!=0) return 6;
        if (iteration>=4) timings[stage?1:0].push_back(elapsed);
    }
    for(auto& t:timings) std::ranges::sort(t);
    const auto host_ms=timings[0][timings[0].size()/2];
    const auto local_ms=timings[1][timings[1].size()/2];
    std::string text=fmt::format("{{\"bytes\":{},\"trials_per_path\":{},\"host_median_ms\":{},\"local_median_ms\":{},\"ratio\":{},\"all_bytes_equal\":true,\"host_ms\":[",
        size,timings[0].size(),host_ms,local_ms,local_ms/host_ms);
    for(size_t i=0;i<timings[0].size();++i)text+=fmt::format("{}{}",i?",":"",timings[0][i]);
    text+="],\"local_ms\":[";
    for(size_t i=0;i<timings[1].size();++i)text+=fmt::format("{}{}",i?",":"",timings[1][i]);
    text+="]}";
    if(report && report_size>0){const auto n=std::min<size_t>(text.size(),size_t(report_size-1));std::memcpy(report,text.data(),n);report[n]=0;}
    return 0;
}
