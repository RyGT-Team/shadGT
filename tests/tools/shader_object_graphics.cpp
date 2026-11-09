// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone NVIDIA graphics shader-object draw with dynamic vertex/topology state.
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::fprintf(stderr, "%s failed %d\n", #x, int(r_)); return 1; } } while (0)
#define LOAD_I(n) auto n = reinterpret_cast<PFN_##n>(get_instance(instance, #n)); if (!n) { std::fprintf(stderr, "missing %s\n", #n); return 1; }
#define LOAD_D(n) auto n = reinterpret_cast<PFN_##n>(get_device(device, #n)); if (!n) { std::fprintf(stderr, "missing %s\n", #n); return 1; }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate); if (!input) return 2;
    const auto bytes = input.tellg(); if (bytes <= 0 || bytes % 4) return 2;
    std::vector<uint32_t> code(size_t(bytes)/4); input.seekg(0); input.read(reinterpret_cast<char*>(code.data()), bytes); if (!input) return 2;
    HMODULE dll = LoadLibraryW(L"vulkan-1.dll"); if (!dll) return 1;
    auto get_instance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(dll,"vkGetInstanceProcAddr")); if (!get_instance) return 1;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(get_instance(nullptr,"vkCreateInstance")); if (!create) return 1;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app; VkInstance instance{}; CHECK(create(&ici,nullptr,&instance));
    LOAD_I(vkEnumeratePhysicalDevices); LOAD_I(vkGetPhysicalDeviceProperties); LOAD_I(vkEnumerateDeviceExtensionProperties);
    LOAD_I(vkGetPhysicalDeviceFeatures2); LOAD_I(vkGetPhysicalDeviceQueueFamilyProperties); LOAD_I(vkGetPhysicalDeviceMemoryProperties);
    LOAD_I(vkCreateDevice); LOAD_I(vkGetDeviceProcAddr); LOAD_I(vkDestroyInstance);
    uint32_t n=0; CHECK(vkEnumeratePhysicalDevices(instance,&n,nullptr)); std::vector<VkPhysicalDevice> all(n); CHECK(vkEnumeratePhysicalDevices(instance,&n,all.data()));
    VkPhysicalDevice physical{}; VkPhysicalDeviceProperties props{};
    for (auto p : all) { vkGetPhysicalDeviceProperties(p,&props); if (props.vendorID==0x10de && props.apiVersion>=VK_API_VERSION_1_3) { physical=p; break; } }
    if (!physical) { std::puts("SKIP no Vulkan 1.3 NVIDIA device"); return 77; }
    n=0; CHECK(vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,nullptr)); std::vector<VkExtensionProperties> extensions(n); CHECK(vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,extensions.data()));
    auto has = [&](const char* name) { for (const auto& e:extensions) if (!std::strcmp(name,e.extensionName)) return true; return false; };
    if (!has(VK_EXT_SHADER_OBJECT_EXTENSION_NAME) || !has(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME)) { std::puts("SKIP required shader-object or vertex-input extension missing"); return 77; }
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertex{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceShaderObjectFeaturesEXT object{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,&vertex};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,&object};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&f13}; vkGetPhysicalDeviceFeatures2(physical,&features);
    if (!object.shaderObject || !vertex.vertexInputDynamicState || !f13.dynamicRendering || !features.features.vertexPipelineStoresAndAtomics) { std::puts("SKIP required features unavailable"); return 77; }
    f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,&object}; f13.dynamicRendering=VK_TRUE;
    object = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,&vertex}; object.shaderObject=VK_TRUE;
    vertex = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT}; vertex.vertexInputDynamicState=VK_TRUE;
    VkPhysicalDeviceFeatures enabled{}; enabled.vertexPipelineStoresAndAtomics=VK_TRUE;
    uint32_t families=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,nullptr); std::vector<VkQueueFamilyProperties> queues(families); vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,queues.data());
    uint32_t family=0; while (family<families && !(queues[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) ++family; if (family==families) return 1;
    float priority=1.0f; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
    const char* names[]{VK_EXT_SHADER_OBJECT_EXTENSION_NAME,VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext=&f13; dci.pEnabledFeatures=&enabled; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qi; dci.enabledExtensionCount=2; dci.ppEnabledExtensionNames=names;
    VkDevice device{}; CHECK(vkCreateDevice(physical,&dci,nullptr,&device)); auto get_device=vkGetDeviceProcAddr;
    LOAD_D(vkDestroyDevice); LOAD_D(vkGetDeviceQueue); LOAD_D(vkCreateBuffer); LOAD_D(vkGetBufferMemoryRequirements); LOAD_D(vkAllocateMemory); LOAD_D(vkBindBufferMemory); LOAD_D(vkMapMemory); LOAD_D(vkUnmapMemory); LOAD_D(vkDestroyBuffer); LOAD_D(vkFreeMemory);
    LOAD_D(vkCreateDescriptorSetLayout); LOAD_D(vkCreateDescriptorPool); LOAD_D(vkAllocateDescriptorSets); LOAD_D(vkUpdateDescriptorSets); LOAD_D(vkCreatePipelineLayout);
    LOAD_D(vkCreateShadersEXT); LOAD_D(vkDestroyShaderEXT); LOAD_D(vkCreateCommandPool); LOAD_D(vkAllocateCommandBuffers); LOAD_D(vkBeginCommandBuffer); LOAD_D(vkEndCommandBuffer);
    LOAD_D(vkCmdBeginRendering); LOAD_D(vkCmdEndRendering); LOAD_D(vkCmdBindShadersEXT); LOAD_D(vkCmdSetVertexInputEXT); LOAD_D(vkCmdSetPrimitiveTopology); LOAD_D(vkCmdSetPrimitiveRestartEnable); LOAD_D(vkCmdSetRasterizerDiscardEnable); LOAD_D(vkCmdSetViewportWithCount); LOAD_D(vkCmdSetScissorWithCount);
    LOAD_D(vkCmdBindDescriptorSets); LOAD_D(vkCmdPushConstants); LOAD_D(vkCmdDraw); LOAD_D(vkCmdPipelineBarrier); LOAD_D(vkQueueSubmit); LOAD_D(vkQueueWaitIdle);
    LOAD_D(vkDestroyCommandPool); LOAD_D(vkDestroyPipelineLayout); LOAD_D(vkDestroyDescriptorPool); LOAD_D(vkDestroyDescriptorSetLayout);
    VkQueue queue{}; vkGetDeviceQueue(device,family,0,&queue);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=24; bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; VkBuffer buffer{}; CHECK(vkCreateBuffer(device,&bi,nullptr,&buffer));
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,buffer,&req); VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    uint32_t type=0; const VkMemoryPropertyFlags wanted=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (type<memory.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) || (memory.memoryTypes[type].propertyFlags&wanted)!=wanted)) ++type; if (type==memory.memoryTypeCount) return 1;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=type; VkDeviceMemory mem{}; CHECK(vkAllocateMemory(device,&ai,nullptr,&mem)); CHECK(vkBindBufferMemory(device,buffer,mem,0));
    void* mapped{}; CHECK(vkMapMemory(device,mem,0,24,0,&mapped)); std::memset(mapped,0,24);
    VkDescriptorSetLayoutBinding bind{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr}; VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; sli.bindingCount=1; sli.pBindings=&bind;
    VkDescriptorSetLayout set_layout{}; CHECK(vkCreateDescriptorSetLayout(device,&sli,nullptr,&set_layout)); VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}; VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets=1; dpi.poolSizeCount=1; dpi.pPoolSizes=&ps;
    VkDescriptorPool pool{}; CHECK(vkCreateDescriptorPool(device,&dpi,nullptr,&pool)); VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dsai.descriptorPool=pool; dsai.descriptorSetCount=1; dsai.pSetLayouts=&set_layout; VkDescriptorSet set{}; CHECK(vkAllocateDescriptorSets(device,&dsai,&set));
    VkDescriptorBufferInfo db{buffer,0,24}; VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet=set; w.dstBinding=0; w.descriptorCount=1; w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo=&db; vkUpdateDescriptorSets(device,1,&w,0,nullptr);
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,8}; VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; li.setLayoutCount=1; li.pSetLayouts=&set_layout; li.pushConstantRangeCount=1; li.pPushConstantRanges=&push; VkPipelineLayout layout{}; CHECK(vkCreatePipelineLayout(device,&li,nullptr,&layout));
    VkShaderCreateInfoEXT si{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT}; si.stage=VK_SHADER_STAGE_VERTEX_BIT; si.codeType=VK_SHADER_CODE_TYPE_SPIRV_EXT; si.codeSize=size_t(bytes); si.pCode=code.data(); si.pName="main"; si.setLayoutCount=1; si.pSetLayouts=&set_layout; si.pushConstantRangeCount=1; si.pPushConstantRanges=&push;
    VkShaderEXT shader{}; CHECK(vkCreateShadersEXT(device,1,&si,nullptr,&shader)); VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; ci.queueFamilyIndex=family; VkCommandPool cp{}; CHECK(vkCreateCommandPool(device,&ci,nullptr,&cp));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool=cp; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1; VkCommandBuffer cmd{}; CHECK(vkAllocateCommandBuffers(device,&cai,&cmd)); VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&cb));
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea={{0,0},{1,1}}; rendering.layerCount=1; vkCmdBeginRendering(cmd,&rendering);
    const std::array<VkShaderStageFlagBits,2> stages{VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_FRAGMENT_BIT}; const std::array<VkShaderEXT,2> shaders{shader,VK_NULL_HANDLE}; vkCmdBindShadersEXT(cmd,2,stages.data(),shaders.data());
    const VkViewport viewport{0,0,1,1,0,1}; const VkRect2D scissor{{0,0},{1,1}};
    vkCmdSetViewportWithCount(cmd,1,&viewport); vkCmdSetScissorWithCount(cmd,1,&scissor);
    vkCmdSetVertexInputEXT(cmd,0,nullptr,0,nullptr); vkCmdSetPrimitiveRestartEnable(cmd,VK_FALSE); vkCmdSetRasterizerDiscardEnable(cmd,VK_TRUE); vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);
    for (uint32_t i=0;i<2;++i) { vkCmdSetPrimitiveTopology(cmd,i ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST); const uint32_t words[]{10+10*i,3*i}; vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_VERTEX_BIT,0,8,words); vkCmdDraw(cmd,3,1,0,0); }
    vkCmdEndRendering(cmd); VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT; vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    CHECK(vkEndCommandBuffer(cmd)); VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cmd; CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    const std::array<uint32_t,6> expected{10,11,12,20,21,22}; if (std::memcmp(mapped,expected.data(),24)) { const auto* p=static_cast<const uint32_t*>(mapped); std::fprintf(stderr,"FAIL output %u %u %u %u %u %u\n",p[0],p[1],p[2],p[3],p[4],p[5]); return 1; }
    vkUnmapMemory(device,mem); vkDestroyCommandPool(device,cp,nullptr); vkDestroyShaderEXT(device,shader,nullptr); vkDestroyPipelineLayout(device,layout,nullptr); vkDestroyDescriptorPool(device,pool,nullptr); vkDestroyDescriptorSetLayout(device,set_layout,nullptr); vkDestroyBuffer(device,buffer,nullptr); vkFreeMemory(device,mem,nullptr); vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr); FreeLibrary(dll);
    std::printf("PASS %s graphics shader-object draws with triangle/point topology and storage-buffer readback\n",props.deviceName);
}
