// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone NVIDIA 4x MSAA resolve: compatible UNORM/SRGB views, a UINT
// sample-zero resolve, and same-format intermediate plus bitwise image copy.
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
#define CHECK(x) do { VkResult r_=(x); if (r_!=VK_SUCCESS) { std::fprintf(stderr,"%s failed %d\n",#x,int(r_)); return 1; } } while(0)
#define LI(n) auto n=reinterpret_cast<PFN_##n>(gi(instance,#n)); if(!n)return 1
#define LD(n) auto n=reinterpret_cast<PFN_##n>(gd(device,#n)); if(!n)return 1
int main() {
    HMODULE dll=LoadLibraryW(L"vulkan-1.dll"); if(!dll)return 1;
    auto gi=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(dll,"vkGetInstanceProcAddr")); if(!gi)return 1;
    auto create=reinterpret_cast<PFN_vkCreateInstance>(gi(nullptr,"vkCreateInstance")); if(!create)return 1;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app; VkInstance instance{}; CHECK(create(&ici,nullptr,&instance));
    LI(vkEnumeratePhysicalDevices); LI(vkGetPhysicalDeviceProperties); LI(vkGetPhysicalDeviceQueueFamilyProperties); LI(vkGetPhysicalDeviceMemoryProperties); LI(vkGetPhysicalDeviceFeatures2); LI(vkCreateDevice); LI(vkGetDeviceProcAddr); LI(vkDestroyInstance);
    uint32_t n=0; CHECK(vkEnumeratePhysicalDevices(instance,&n,nullptr)); std::vector<VkPhysicalDevice> all(n); CHECK(vkEnumeratePhysicalDevices(instance,&n,all.data()));
    VkPhysicalDevice physical{}; VkPhysicalDeviceProperties properties{}; for(auto p:all){vkGetPhysicalDeviceProperties(p,&properties);if(properties.vendorID==0x10de && properties.apiVersion>=VK_API_VERSION_1_3){physical=p;break;}}
    if(!physical){std::puts("SKIP no NVIDIA Vulkan 1.3");return 77;}
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&f13}; vkGetPhysicalDeviceFeatures2(physical,&f2);
    if(!f13.dynamicRendering){std::puts("SKIP dynamic rendering unavailable");return 77;} f13={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; f13.dynamicRendering=VK_TRUE;
    n=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr); std::vector<VkQueueFamilyProperties> qs(n); vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,qs.data()); uint32_t family=0; while(family<n && !(qs[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family; if(family==n)return 1;
    float priority=1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.pNext=&f13;di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;VkDevice device{};CHECK(vkCreateDevice(physical,&di,nullptr,&device)); auto gd=vkGetDeviceProcAddr;
    LD(vkDestroyDevice);LD(vkGetDeviceQueue);LD(vkCreateImage);LD(vkDestroyImage);LD(vkGetImageMemoryRequirements);LD(vkCreateImageView);LD(vkDestroyImageView);LD(vkAllocateMemory);LD(vkFreeMemory);LD(vkBindImageMemory);LD(vkCreateBuffer);LD(vkDestroyBuffer);LD(vkGetBufferMemoryRequirements);LD(vkBindBufferMemory);LD(vkMapMemory);LD(vkUnmapMemory);
    LD(vkCreateCommandPool);LD(vkDestroyCommandPool);LD(vkAllocateCommandBuffers);LD(vkResetCommandBuffer);LD(vkBeginCommandBuffer);LD(vkEndCommandBuffer);LD(vkCmdPipelineBarrier);LD(vkCmdBeginRendering);LD(vkCmdEndRendering);LD(vkCmdResolveImage);LD(vkCmdCopyImage);LD(vkCmdCopyImageToBuffer);LD(vkQueueSubmit);LD(vkQueueWaitIdle);
    VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);VkPhysicalDeviceMemoryProperties mp{};vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    auto memory_type=[&](uint32_t bits,VkMemoryPropertyFlags flags){for(uint32_t i=0;i<mp.memoryTypeCount;++i)if((bits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&flags)==flags)return i;return UINT32_MAX;};
    struct Image{VkImage image{};VkDeviceMemory memory{};VkImageView view{};};
    auto image=[&](VkFormat format,VkSampleCountFlagBits samples,VkFormat view_format,bool make_view,Image& out)->bool{
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={1,1,1};ci.mipLevels=1;ci.arrayLayers=1;ci.samples=samples;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if(vkCreateImage(device,&ci,nullptr,&out.image)!=VK_SUCCESS)return false;VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,out.image,&req);uint32_t type=memory_type(req.memoryTypeBits,0);if(type==UINT32_MAX)return false;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;if(vkAllocateMemory(device,&ai,nullptr,&out.memory)!=VK_SUCCESS || vkBindImageMemory(device,out.image,out.memory,0)!=VK_SUCCESS)return false;
        if(make_view){VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=out.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=view_format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};if(vkCreateImageView(device,&vi,nullptr,&out.view)!=VK_SUCCESS)return false;}return true;};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=16;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;VkBuffer buffer{};CHECK(vkCreateBuffer(device,&bi,nullptr,&buffer));VkMemoryRequirements br{};vkGetBufferMemoryRequirements(device,buffer,&br);uint32_t bt=memory_type(br.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);if(bt==UINT32_MAX)return 1;
    VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};bai.allocationSize=br.size;bai.memoryTypeIndex=bt;VkDeviceMemory bm{};CHECK(vkAllocateMemory(device,&bai,nullptr,&bm));CHECK(vkBindBufferMemory(device,buffer,bm,0));void* mapped{};CHECK(vkMapMemory(device,bm,0,16,0,&mapped));
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pci.queueFamilyIndex=family;VkCommandPool pool{};CHECK(vkCreateCommandPool(device,&pci,nullptr,&pool));VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;VkCommandBuffer cmd{};CHECK(vkAllocateCommandBuffers(device,&cai,&cmd));
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    for(int mode=0;mode<3;++mode){
        const bool integer=mode==2, intermediate=mode==1;const VkFormat src_format=integer?VK_FORMAT_R8G8B8A8_UINT:VK_FORMAT_R8G8B8A8_UNORM;const VkFormat dst_format=integer?src_format:VK_FORMAT_R8G8B8A8_SRGB;
        Image src{},dst{},tmp{};if(!image(src_format,VK_SAMPLE_COUNT_4_BIT,src_format,true,src)||!image(dst_format,VK_SAMPLE_COUNT_1_BIT,src_format,!intermediate,dst)||(intermediate&&!image(src_format,VK_SAMPLE_COUNT_1_BIT,src_format,false,tmp))){std::fprintf(stderr,"image creation failed for case %d\n",mode);return 1;}
        CHECK(vkResetCommandBuffer(cmd,0));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(cmd,&begin));
        std::array<VkImageMemoryBarrier,3> init{}; const std::array<VkImage,3> imgs{src.image,dst.image,tmp.image};const uint32_t count=intermediate?3:2;
        for(uint32_t i=0;i<count;++i){init[i].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;init[i].srcAccessMask=0;init[i].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;init[i].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;init[i].newLayout=VK_IMAGE_LAYOUT_GENERAL;init[i].srcQueueFamilyIndex=init[i].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;init[i].image=imgs[i];init[i].subresourceRange=range;}
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,count,init.data());
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};att.imageView=src.view;att.imageLayout=VK_IMAGE_LAYOUT_GENERAL;att.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;att.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        if(integer){att.clearValue.color.uint32[0]=12;att.clearValue.color.uint32[1]=34;att.clearValue.color.uint32[2]=56;att.clearValue.color.uint32[3]=78;}else{att.clearValue.color.float32[0]=0.25f;att.clearValue.color.float32[1]=0.5f;att.clearValue.color.float32[2]=0.75f;att.clearValue.color.float32[3]=1.0f;}
        if(!intermediate){att.resolveMode=integer?VK_RESOLVE_MODE_SAMPLE_ZERO_BIT:VK_RESOLVE_MODE_AVERAGE_BIT;att.resolveImageView=dst.view;att.resolveImageLayout=VK_IMAGE_LAYOUT_GENERAL;}
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea={{0,0},{1,1}};ri.layerCount=1;ri.colorAttachmentCount=1;ri.pColorAttachments=&att;vkCmdBeginRendering(cmd,&ri);vkCmdEndRendering(cmd);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        if(intermediate){VkImageResolve r{};r.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};r.dstSubresource=r.srcSubresource;r.extent={1,1,1};vkCmdResolveImage(cmd,src.image,VK_IMAGE_LAYOUT_GENERAL,tmp.image,VK_IMAGE_LAYOUT_GENERAL,1,&r);
            barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            VkImageCopy c{};c.srcSubresource=r.srcSubresource;c.dstSubresource=r.dstSubresource;c.extent=r.extent;vkCmdCopyImage(cmd,tmp.image,VK_IMAGE_LAYOUT_GENERAL,dst.image,VK_IMAGE_LAYOUT_GENERAL,1,&c);
            barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);}
        VkBufferImageCopy read{};read.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};read.imageExtent={1,1,1};vkCmdCopyImageToBuffer(cmd,dst.image,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&read);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);CHECK(vkEndCommandBuffer(cmd));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
        const auto* p=static_cast<const unsigned char*>(mapped);const std::array<unsigned char,4> expected=integer?std::array<unsigned char,4>{12,34,56,78}:std::array<unsigned char,4>{64,127,191,255};
        const bool match = integer ? std::memcmp(p,expected.data(),4)==0 :
            p[0]==64 && (p[1]==127 || p[1]==128) && p[2]==191 && p[3]==255;
        if(!match){std::fprintf(stderr,"case %d got %u,%u,%u,%u\n",mode,p[0],p[1],p[2],p[3]);return 1;}
        std::printf("PASS %s: %u,%u,%u,%u\n",integer?"UINT sample-zero":intermediate?"UNORM intermediate to SRGB":"UNORM source view to SRGB",p[0],p[1],p[2],p[3]);
        for(auto& x:{&src,&dst,&tmp}){if(x->view)vkDestroyImageView(device,x->view,nullptr);if(x->image)vkDestroyImage(device,x->image,nullptr);if(x->memory)vkFreeMemory(device,x->memory,nullptr);}
    }
    vkUnmapMemory(device,bm);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,bm,nullptr);vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);FreeLibrary(dll);std::printf("PASS %s resolve fixture\n",properties.deviceName);return 0;
}
