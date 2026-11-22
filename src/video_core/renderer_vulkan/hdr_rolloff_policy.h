// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>
#include <cstdlib>

namespace Vulkan {

// FIX-SGT-113: no roll-off unless asked for, in both games: each tone maps to the display's peak
// itself, GT7 to the one the system reports (FIX-GT7-255) and GT Sport to the one calibrated in
// its own HDR Peak Brightness screen, which a second curve throws off.
inline constexpr float HdrRolloffDefaultNits = 0.0f;

// FIX-GT7-255: the peak in nits the post-process pass rolls an HDR picture's highlights off to
// (0: no roll-off). `text` is SHADGT_HDR_ROLLOFF_NITS, 0..10000; anything else leaves `configured`
// (GPU.hdr_peak_nits, else HdrRolloffDefaultNits).
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
