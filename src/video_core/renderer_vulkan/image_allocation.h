// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <vk_mem_alloc.h>
namespace Vulkan {
VmaPool SmallImageAllocationPool(VmaAllocator allocator, const VkImageCreateInfo& image,
                                const VmaAllocationCreateInfo& allocation);
void DestroySmallImageAllocationPools(VmaAllocator allocator);
/// PERF-GTS-466: a pool for the native renderer's upload buffers that keeps its memory, or no
/// pool. Off unless RYGT_NATIVE_UPLOAD_POOL_MB is set (the memory it keeps, in 256 MB blocks).
VmaPool UploadBufferPool(VmaAllocator allocator, const VkBufferCreateInfo& buffer,
                         const VmaAllocationCreateInfo& allocation);
void DestroyUploadBufferPools(VmaAllocator allocator);
}
namespace VideoCore {
void ClearRecycledImages(VmaAllocator allocator);
}
