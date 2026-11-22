// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>
#include <cstdlib>

namespace Vulkan {

// FIX-GT7-255: the peak in nits the post-process pass rolls an HDR picture's highlights off to
// (0: no roll-off). `text` is SHADGT_HDR_ROLLOFF_NITS, 0..10000; anything else leaves `configured`
// (GPU.hdr_peak_nits, else the title's default, TitleDefaults::hdr_rolloff_nits).
inline float HdrRolloffNits(const char* text, float configured) {
    if (text == nullptr) {
        return configured;
    }
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value) || value < 0.0f || value > 10000.0f) {
        return configured;
    }
    return value;
}

} // namespace Vulkan
