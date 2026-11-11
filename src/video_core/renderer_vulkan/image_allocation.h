// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <vk_mem_alloc.h>
namespace Vulkan {
VmaPool SmallImageAllocationPool(VmaAllocator allocator, const VkImageCreateInfo& image,
                                const VmaAllocationCreateInfo& allocation);
void DestroySmallImageAllocationPools(VmaAllocator allocator);
}
namespace VideoCore {
void ClearRecycledImages(VmaAllocator allocator);
}
