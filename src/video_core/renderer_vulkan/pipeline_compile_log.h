// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

// DIAG-SHC-001: RYGT_PIPELINE_COMPILE_LOG. The GPU tools cannot see the driver compiling: it runs
// on the driver's CPU threads, so in RGP a compile is only a gap. With the switch set, every
// pipeline and shader object the driver builds gets a row in logs/pipeline_compiles.csv (driver
// time, driver cache hit or miss, per stage times, and with 2 the driver's statistics for the
// compiled code), and the first draw that binds it gets an RGP/RenderDoc label saying what it cost.
namespace Vulkan::PipelineCompileLog {

/// 0 = off, 1 = timing and labels, 2 = also the driver's statistics. Read once.
int Level();

/// Level 2 needs VK_KHR_pipeline_executable_properties; call while choosing device extensions.
bool WantsExecutableProperties();

/// Swaps the create, name and bind entry points in the default dispatcher for logging ones.
/// Call right after VULKAN_HPP_DEFAULT_DISPATCHER.init(device); does nothing when the switch is off.
void Install(vk::Device device, bool executable_properties);

} // namespace Vulkan::PipelineCompileLog
