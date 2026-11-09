// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone transfer fixture: the production journal selects an alternating fill
// at a four-byte offset, then Vulkan uploads it to an image and reads it back.
#define VK_NO_PROTOTYPES
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include "ryty_gpu/native/recent_fills.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
#define CHECK(x) do { const auto r=(x); if(r!=VK_SUCCESS) { std::printf("%s: %d\n",#x,int(r)); return 1; } } while(0)
#define LI(n) auto n=reinterpret_cast<PFN_##n>(gi(instance,#n)); if(!n)return 1
#define LD(n) auto n=reinterpret_cast<PFN_##n>(gd(device,#n)); if(!n)return 1
int main() {
    HMODULE dll=LoadLibraryW(L"vulkan-1.dll"); if(!dll)return 1;
    auto gi=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(dll,"vkGetInstanceProcAddr")); if(!gi)return 1;
    auto create=reinterpret_cast<PFN_vkCreateInstance>(gi(nullptr,"vkCreateInstance")); if(!create)return 1;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&app; VkInstance instance{}; CHECK(create(&ci,nullptr,&instance));
    LI(vkEnumeratePhysicalDevices); LI(vkGetPhysicalDeviceProperties); LI(vkGetPhysicalDeviceQueueFamilyProperties); LI(vkGetPhysicalDeviceMemoryProperties); LI(vkCreateDevice); LI(vkGetDeviceProcAddr); LI(vkDestroyInstance);
    uint32_t count=0; CHECK(vkEnumeratePhysicalDevices(instance,&count,nullptr)); std::vector<VkPhysicalDevice> devices(count); CHECK(vkEnumeratePhysicalDevices(instance,&count,devices.data()));
    VkPhysicalDevice physical{}; VkPhysicalDeviceProperties props{}; for(auto p:devices){vkGetPhysicalDeviceProperties(p,&props);if(props.vendorID==0x10de){physical=p;break;}} if(!physical)return 77;
    count=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr); std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data()); uint32_t family=0; while(family<count && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family; if(family==count)return 1;
    float priority=1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;VkDevice device{};CHECK(vkCreateDevice(physical,&di,nullptr,&device)); auto gd=vkGetDeviceProcAddr;
    LD(vkDestroyDevice); LD(vkGetDeviceQueue); LD(vkCreateBuffer); LD(vkDestroyBuffer); LD(vkGetBufferMemoryRequirements); LD(vkCreateImage); LD(vkDestroyImage); LD(vkGetImageMemoryRequirements); LD(vkAllocateMemory); LD(vkFreeMemory); LD(vkBindBufferMemory); LD(vkBindImageMemory); LD(vkMapMemory); LD(vkUnmapMemory); LD(vkCreateCommandPool); LD(vkDestroyCommandPool); LD(vkAllocateCommandBuffers); LD(vkBeginCommandBuffer); LD(vkEndCommandBuffer); LD(vkCmdPipelineBarrier); LD(vkCmdCopyBufferToImage); LD(vkCmdCopyImageToBuffer); LD(vkQueueSubmit); LD(vkQueueWaitIdle);
    VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    auto type=[&](uint32_t bits,VkMemoryPropertyFlags flags){for(uint32_t i=0;i<mp.memoryTypeCount;++i)if((bits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&flags)==flags)return i;return UINT32_MAX;};
    constexpr uint32_t pixels=64, bytes=pixels*4;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=bytes*2;bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;VkBuffer buffer{};CHECK(vkCreateBuffer(device,&bi,nullptr,&buffer));
    VkMemoryRequirements br{};vkGetBufferMemoryRequirements(device,buffer,&br);VkMemoryAllocateInfo ba{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ba.allocationSize=br.size;ba.memoryTypeIndex=type(br.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);if(ba.memoryTypeIndex==UINT32_MAX)return 1;VkDeviceMemory bm{};CHECK(vkAllocateMemory(device,&ba,nullptr,&bm));CHECK(vkBindBufferMemory(device,buffer,bm,0));void* mapped{};CHECK(vkMapMemory(device,bm,0,br.size,0,&mapped));
    Native::RecentFills fills;fills.Record(0x1000,bytes+4,{0x11223344,0x55667788},1,1);auto fill=fills.Find(0x1004,bytes,1);if(!fill)return 1;
    auto* words=static_cast<uint32_t*>(mapped);for(uint32_t i=0;i<pixels;++i)words[i]=fill->pattern[i%2];std::memset(words+pixels,0,bytes);
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_R8G8B8A8_UNORM;ii.extent={8,8,1};ii.mipLevels=1;ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;ii.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;VkImage image{};CHECK(vkCreateImage(device,&ii,nullptr,&image));VkMemoryRequirements ir{};vkGetImageMemoryRequirements(device,image,&ir);VkMemoryAllocateInfo ia{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ia.allocationSize=ir.size;ia.memoryTypeIndex=type(ir.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);if(ia.memoryTypeIndex==UINT32_MAX)return 1;VkDeviceMemory im{};CHECK(vkAllocateMemory(device,&ia,nullptr,&im));CHECK(vkBindImageMemory(device,image,im,0));
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=family;VkCommandPool pool{};CHECK(vkCreateCommandPool(device,&pi,nullptr,&pool));VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;VkCommandBuffer cmd{};CHECK(vkAllocateCommandBuffers(device,&ai,&cmd));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(cmd,&begin));
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;ib.newLayout=VK_IMAGE_LAYOUT_GENERAL;ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ib.image=image;ib.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};ib.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={8,8,1};vkCmdCopyBufferToImage(cmd,buffer,image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,nullptr,0,nullptr);copy.bufferOffset=bytes;vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&copy);mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);CHECK(vkEndCommandBuffer(cmd));VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;CHECK(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
    bool ok=true;for(uint32_t i=0;i<pixels;++i)ok &= words[pixels+i]==fill->pattern[i%2];std::printf("%s %s 4-byte-offset image upload: %08x %08x\n",ok?"PASS":"FAIL",props.deviceName,words[pixels],words[pixels+1]);
    vkUnmapMemory(device,bm);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,im,nullptr);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,bm,nullptr);vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);FreeLibrary(dll);return ok?0:1;
}
