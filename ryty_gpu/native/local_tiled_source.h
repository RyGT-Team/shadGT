// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ryty_gpu/native/image_registry.h"
#include "ryty_gpu/native/local_tiled_source_policy.h"
namespace Native {
inline ImageRegistry::UploadSource LocalTiledSource(
    ImageRegistry& registry, Rhi::CommandList& list, ImageRegistry::UploadSource source,
    u64 size, bool tiled, u32 vendor) {
    if (!source.buffer || !PreferLocalTiledSource(size,tiled,vendor,source.kind,
                                                 source.offset,source.buffer->Size())) return source;
    auto* local = registry.AcquireUploadGatherScratch(list,size);
    if (!local) return source; // Optional optimization: keep the complete original source.
    list.Barrier(Rhi::ScopeHost | Rhi::ScopeDraw | Rhi::ScopeCompute | Rhi::ScopeTransfer,
                 Rhi::ScopeTransfer);
    list.CopyBuffer(source.buffer,source.offset,local,0,size);
    list.Barrier(Rhi::ScopeTransfer,Rhi::ScopeCompute | Rhi::ScopeTransfer);
    source.buffer = local; source.offset = 0;
    // CPU-frozen data remains kind 4; imported guest data is now a GPU gather.
    if (source.kind == 2) source.kind = 3;
    return source; // Original keep-alive buffers survive through GPU completion.
}
}
