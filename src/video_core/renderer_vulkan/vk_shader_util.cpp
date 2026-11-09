// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {
std::mutex code_mutex;
std::unordered_map<VkDevice, std::unordered_map<VkShaderModule, std::vector<u32>>> module_code;
} // namespace

std::vector<u32> ModuleCode(vk::Device device, vk::ShaderModule module) {
    std::scoped_lock lock{code_mutex};
    const auto by_device = module_code.find(static_cast<VkDevice>(device));
    if (by_device == module_code.end()) return {};
    const auto it = by_device->second.find(static_cast<VkShaderModule>(module));
    return it != by_device->second.end() ? it->second : std::vector<u32>{};
}

void ForgetModuleCode(vk::Device device, vk::ShaderModule module) {
    std::scoped_lock lock{code_mutex};
    if (const auto it = module_code.find(static_cast<VkDevice>(device)); it != module_code.end())
        it->second.erase(static_cast<VkShaderModule>(module));
}

void ClearModuleCode(vk::Device device) {
    std::scoped_lock lock{code_mutex};
    module_code.erase(static_cast<VkDevice>(device));
}

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device, bool keep_code) {
    const vk::ShaderModuleCreateInfo shader_info = {
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    };

    auto [module_result, module] = device.createShaderModule(shader_info);
    ASSERT_MSG(module_result == vk::Result::eSuccess, "Failed to compile SPIR-V shader: {}",
               vk::to_string(module_result));
    if (keep_code) {
        try {
            std::scoped_lock lock{code_mutex};
            module_code[static_cast<VkDevice>(device)][static_cast<VkShaderModule>(module)].assign(code.begin(), code.end());
        } catch (...) {
            device.destroyShaderModule(module);
            throw;
        }
    }
    return module;
}

} // namespace Vulkan
