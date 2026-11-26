// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <charconv>
#include <cstdlib>
#include <string_view>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::RytyUpscale {

struct GuestDlssLayout {
    vk::Extent2D scene{}, output{};

    bool Wide() const {
        return output.width * 9 > output.height * 16 + output.height / 10;
    }
};

inline const GuestDlssLayout& DlssLayout() {
    static const auto layout = [] {
        const char* enabled = std::getenv("RYGT_DLSS_GUEST_OUTPUT");
        if (!enabled || std::string_view(enabled) != "1") return GuestDlssLayout{};
        const auto extent = [](const char* name) -> vk::Extent2D {
            const char* value = std::getenv(name);
            if (!value) return {};
            const char* end = value + std::string_view(value).size();
            u32 w{}, h{};
            const auto x = std::from_chars(value, end, w);
            if (x.ec != std::errc{} || x.ptr == end || *x.ptr != 'x') return {};
            const auto y = std::from_chars(x.ptr + 1, end, h);
            if (y.ec != std::errc{} || y.ptr != end || !w || !h || w > 7680 || h > 2160 ||
                u64(w) * h > 3840u * 2160) return {};
            return {w, h};
        };
        const auto scene = extent("RYGT_DLSS_SCENE_SIZE");
        const auto output = extent("RYGT_DLSS_OUTPUT_SIZE");
        if (scene.width && output.width && scene.width <= output.width && scene.height <= output.height)
            return GuestDlssLayout{scene, output};
        return GuestDlssLayout{};
    }();
    return layout;
}

} // namespace Vulkan::RytyUpscale
