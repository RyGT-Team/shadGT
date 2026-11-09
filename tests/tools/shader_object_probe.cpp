// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone probe: create an NVIDIA compute shader object from SPIR-V, obtain
// its driver binary, and create another shader object from that binary.
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#define CHECK(x) do { VkResult r = (x); if (r != VK_SUCCESS) { std::fprintf(stderr, "%s: %d\n", #x, int(r)); return 1; } } while (0)
#define LOAD_I(n) auto n = reinterpret_cast<PFN_##n>(get_instance(instance, #n)); if (!n) return 1
#define LOAD_D(n) auto n = reinterpret_cast<PFN_##n>(get_device(device, #n)); if (!n) return 1
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    if (!file) return 2;
    const auto bytes = file.tellg();
    if (bytes <= 0 || bytes % 4) return 2;
    std::vector<uint32_t> code(size_t(bytes) / 4);
    file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), bytes);
    if (!file) return 2;
    HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (!loader) return 1;
    auto get_instance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    if (!get_instance) return 1;
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get_instance(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    VkInstance instance{}; CHECK(create_instance(&ici, nullptr, &instance));
    LOAD_I(vkEnumeratePhysicalDevices); LOAD_I(vkGetPhysicalDeviceProperties);
    LOAD_I(vkEnumerateDeviceExtensionProperties); LOAD_I(vkGetPhysicalDeviceFeatures2);
    LOAD_I(vkGetPhysicalDeviceQueueFamilyProperties); LOAD_I(vkCreateDevice);
    LOAD_I(vkGetDeviceProcAddr); LOAD_I(vkDestroyInstance);
    uint32_t count = 0; CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count); CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice physical{}; VkPhysicalDeviceProperties props{};
    for (auto candidate : devices) { vkGetPhysicalDeviceProperties(candidate, &props); if (props.vendorID == 0x10de) { physical = candidate; break; } }
    if (!physical) { std::puts("SKIP: NVIDIA device unavailable"); vkDestroyInstance(instance, nullptr); return 77; }
    uint32_t n = 0; CHECK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr));
    std::vector<VkExtensionProperties> ext(n); CHECK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, ext.data()));
    bool supported = false; for (const auto& e : ext) supported |= std::strcmp(e.extensionName, VK_EXT_SHADER_OBJECT_EXTENSION_NAME) == 0;
    VkPhysicalDeviceShaderObjectFeaturesEXT feature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &feature};
    if (supported) vkGetPhysicalDeviceFeatures2(physical, &query);
    if (!supported || !feature.shaderObject) { std::printf("SKIP: %s lacks VK_EXT_shader_object\n", props.deviceName); vkDestroyInstance(instance, nullptr); return 77; }
    uint32_t families = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> queues(families); vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, queues.data());
    uint32_t family = 0; while (family < families && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    if (family == families) return 1;
    float priority = 1.0f; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
    const char* name = VK_EXT_SHADER_OBJECT_EXTENSION_NAME; VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &feature; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = &name;
    VkDevice device{}; CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
    auto get_device = vkGetDeviceProcAddr;
    LOAD_D(vkCreateShadersEXT); LOAD_D(vkGetShaderBinaryDataEXT); LOAD_D(vkDestroyShaderEXT); LOAD_D(vkDestroyDevice);
    VkShaderCreateInfoEXT sci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT}; sci.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    sci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT; sci.codeSize = size_t(bytes); sci.pCode = code.data(); sci.pName = "main";
    VkShaderEXT shader{}; CHECK(vkCreateShadersEXT(device, 1, &sci, nullptr, &shader));
    size_t size = 0; CHECK(vkGetShaderBinaryDataEXT(device, shader, &size, nullptr));
    std::vector<unsigned char> binary(size); CHECK(vkGetShaderBinaryDataEXT(device, shader, &size, binary.data()));
    vkDestroyShaderEXT(device, shader, nullptr); shader = VK_NULL_HANDLE;
    struct alignas(16) BinaryBlock { unsigned char bytes[16]; };
    std::vector<BinaryBlock> aligned((size + 15) / 16); std::memcpy(aligned.data(), binary.data(), size);
    sci.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT; sci.codeSize = size; sci.pCode = aligned.data();
    CHECK(vkCreateShadersEXT(device, 1, &sci, nullptr, &shader));
    vkDestroyShaderEXT(device, shader, nullptr); vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);
    std::printf("PASS: %s SPIR-V and %zu-byte binary shader objects\n", props.deviceName, size);
}
