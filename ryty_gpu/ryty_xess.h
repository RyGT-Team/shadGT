// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ryGT XESS-200: Intel XeSS Super Resolution as a temporal upscaler of the captured scene
// (GtsDlssPass), through Intel's libxess.dll from the XeSS SDK (headers in ryty_gpu/xess_api,
// Intel Simplified Software License). It runs on any GPU with the needed Vulkan features (on AMD
// and NVIDIA through XeSS's cross-vendor DP4a path). Chosen with GPU.fsr_upscaler "xess" (or
// SHADGT_FSR_UPSCALER=xess), with FSR and temporal FSR on; the DLL is loaded from next to
// GTSport.exe and only then are XeSS's Vulkan extensions and features enabled.

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "ryty_gpu/ryty_fsr3.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::RytyXess {

/// The launcher picked XeSS for the temporal upscaler (no DLL check).
bool Requested();

/// Loads libxess.dll when XeSS is requested; false when it is not or the DLL is missing.
bool Load();

/// The instance and device extensions XeSS needs (called while the renderer creates them).
void AddInstanceExtensions(std::vector<const char*>& extensions,
                           const std::vector<vk::ExtensionProperties>& available);
void AddDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical_device,
                         const std::function<bool(std::string_view)>& add_extension);

/// "XeSS 3.0.2", for the window title.
std::string VersionName();

class Temporal {
public:
    ~Temporal();

    /// Records XeSS for one frame (FSR's frame description; XeSS uses its image views). False
    /// when it failed; the caller then keeps the game's TAA.
    bool Evaluate(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device,
                  vk::CommandBuffer cmdbuf, const RytyFsr3::TemporalFrame& frame);

private:
    void* context{}; // xess_context_handle_t
    vk::Extent2D context_input{};
    vk::Extent2D context_output{};
};

} // namespace Vulkan::RytyXess
