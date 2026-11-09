// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_common.h"
#include "ryty_gpu/image_lifecycle.h"

// Implement vma functions
#define VMA_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#include <vk_mem_alloc.h>
#pragma clang diagnostic pop

// Store the dispatch loader here
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include <atomic>
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include "video_core/renderer_vulkan/image_allocation.h"
namespace Vulkan {
namespace {
std::mutex small_image_pools_mutex;
using ImagePoolKey = std::array<uint32_t, 17>;
std::map<VmaAllocator, std::map<std::pair<uint32_t, VkDeviceSize>, VmaPool>> small_image_pools;
std::map<VmaAllocator, std::map<ImagePoolKey, VmaPool>> small_image_pool_cache;
}
VmaPool SmallImageAllocationPool(VmaAllocator allocator, const VkImageCreateInfo& image,
                                const VmaAllocationCreateInfo& allocation) {
    if (image.pNext || image.sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        (image.flags & VK_IMAGE_CREATE_DISJOINT_BIT) ||
        (image.tiling != VK_IMAGE_TILING_OPTIMAL && image.tiling != VK_IMAGE_TILING_LINEAR) ||
        (allocation.flags & VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT))
        return VK_NULL_HANDLE;
    const ImagePoolKey key{image.flags, uint32_t(image.imageType), uint32_t(image.format),
        image.extent.width, image.extent.height, image.extent.depth, image.mipLevels,
        image.arrayLayers, uint32_t(image.samples), uint32_t(image.tiling), image.usage,
        uint32_t(image.sharingMode), allocation.flags, uint32_t(allocation.usage),
        allocation.requiredFlags, allocation.preferredFlags, allocation.memoryTypeBits};
    std::scoped_lock lock{small_image_pools_mutex};
    auto& cache = small_image_pool_cache[allocator];
    if (const auto found = cache.find(key); found != cache.end()) return found->second;
    ImageLife::Scope query{"image_memory_requirements", image.extent.width, image.extent.height,
                           static_cast<std::uint64_t>(image.format), image.usage,
                           (std::uint64_t(image.arrayLayers) << 32) | image.mipLevels};
    // Query once per complete resource configuration: compressed images must be
    // classified by their allocation footprint, not uncompressed pixel dimensions.
    const auto& functions = allocator->GetVulkanFunctions();
    VkMemoryRequirements requirements{};
    bool requires_dedicated = false;
#if VMA_KHR_MAINTENANCE4 || VMA_VULKAN_VERSION >= 1003000
    if (functions.vkGetDeviceImageMemoryRequirements) {
        const VkDeviceImageMemoryRequirements info{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, nullptr, &image};
        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 result{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated};
        functions.vkGetDeviceImageMemoryRequirements(allocator->m_hDevice, &info, &result);
        requirements = result.memoryRequirements;
        requires_dedicated = dedicated.requiresDedicatedAllocation;
    } else
#endif
    {
        VkImage temporary{};
        if (functions.vkCreateImage(allocator->m_hDevice, &image, allocator->GetAllocationCallbacks(), &temporary) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        bool prefers_dedicated = false;
        allocator->GetImageMemoryRequirements(temporary, requirements, requires_dedicated, prefers_dedicated);
        functions.vkDestroyImage(allocator->m_hDevice, temporary, allocator->GetAllocationCallbacks());
    }
    const auto footprint = std::max(requirements.size, requirements.alignment);
    query.Finish(footprint, requires_dedicated);
    if (requires_dedicated || footprint > (4ULL << 20)) {
        cache.emplace(key, VK_NULL_HANDLE);
        return VK_NULL_HANDLE;
    }
    uint32_t type = 0;
    if (allocator->FindMemoryTypeIndex(requirements.memoryTypeBits, &allocation,
            VmaBufferImageUsage(image), &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    const VkDeviceSize block_size = footprint <= (64ULL << 10) ? 1ULL << 20 :
        footprint <= (1ULL << 20) ? 4ULL << 20 : 8ULL << 20;
    auto& pool = small_image_pools[allocator][{type, block_size}];
    if (!pool) {
        const VmaPoolCreateInfo info{.memoryTypeIndex = type, .blockSize = block_size};
        if (vmaCreatePool(allocator, &info, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
        vmaSetPoolName(allocator, pool, block_size == (1ULL << 20) ? "Tiny images" :
            block_size == (4ULL << 20) ? "Small images" : "Medium images");
    }
    cache.emplace(key, pool);
    return pool;
}
void DestroySmallImageAllocationPools(VmaAllocator allocator) {
    std::scoped_lock lock{small_image_pools_mutex};
    small_image_pool_cache.erase(allocator);
    const auto it = small_image_pools.find(allocator);
    if (it == small_image_pools.end()) return;
    for (const auto& [type, pool] : it->second) if (pool) vmaDestroyPool(allocator, pool);
    small_image_pools.erase(it);
}
static std::atomic<vk::DeviceSize> g_empty_vma_bytes_freed{};
vk::DeviceSize EmptyVmaBytesFreed() {
    return g_empty_vma_bytes_freed.load(std::memory_order_relaxed);
}
vk::DeviceSize TrimEmptyVmaBlocks(VmaAllocator allocator) {
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(allocator, &props);
    vk::DeviceSize freed = 0;
    for (uint32_t i = 0; i < props->memoryTypeCount; ++i) {
        if (!(props->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
        if (auto* blocks = allocator->m_pBlockVectors[i]) freed += blocks->TrimEmptyBlocks();
    }
    g_empty_vma_bytes_freed.fetch_add(freed, std::memory_order_relaxed);
    return freed;
}
}
