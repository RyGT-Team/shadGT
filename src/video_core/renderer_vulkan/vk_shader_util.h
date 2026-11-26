// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/**
 * @brief Creates a vulkan shader module from SPIR-V bytecode.
 * @param code The SPIR-V bytecode data.
 * @param device The vulkan device handle
 */
vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device, bool keep_code = false);

std::vector<u32> ModuleCode(vk::Device device, vk::ShaderModule module);
/// shadgt7 form: looks the module up on every device (handles are unique per device).
std::vector<u32> ModuleCode(vk::ShaderModule module);
void ForgetModuleCode(vk::Device device, vk::ShaderModule module);
/// FIX-SGT-192: a module's SPIR-V hash (never 0; 0 when the module is not known). CompileSPV
/// records it; a module made another way is recorded with RememberModuleHash.
void RememberModuleHash(vk::ShaderModule module, std::span<const u32> code);
u64 ModuleHash(vk::ShaderModule module);
void ClearModuleCode(vk::Device device);

} // namespace Vulkan
