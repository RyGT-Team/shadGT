// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

enum class ReadbackOrder { Rgba, Bgra };

// Convert tightly packed pixels to opaque 8-bit SDR. Source and output may be the same buffer.
bool ConvertReadbackPixels(std::span<const u8> source, std::span<u8> output, vk::Format format,
                           bool hdr_encoded, ReadbackOrder order);

} // namespace Vulkan
