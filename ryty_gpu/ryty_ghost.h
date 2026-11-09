// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <string_view>

#include "common/ryty_config.h"
#include "video_core/amdgpu/regs.h"

namespace Vulkan::RytyGhost {

inline bool IsGhostCoverageDraw(const AmdGpu::Regs& regs) {
    const u32 first = regs.reg_array[0xa30e];
    const u32 second = regs.reg_array[0xa30f];
    const bool ghost_mask = (first == 0xcccc9999u && second == 0x33336666u) ||
                            (first == 0x33336666u && second == 0xcccc9999u);
    if (!ghost_mask) {
        return false;
    }
    const auto& scene = regs.color_buffers[0];
    const auto& velocity = regs.color_buffers[1];
    const auto scene_format = scene.GetDataFmt();
    return regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Normal &&
           regs.color_control.rop3 == AmdGpu::ColorControl::LogicOp::Copy &&
           regs.depth_control.depth_enable && regs.depth_buffer.NumSamples() == 1 && scene &&
           velocity && scene.NumSamples() == 1 && velocity.NumSamples() == 1 &&
           (scene_format == AmdGpu::DataFormat::Format10_11_11 ||
            scene_format == AmdGpu::DataFormat::Format11_11_10) &&
           velocity.GetDataFmt() == AmdGpu::DataFormat::Format16_16 &&
           scene.GetNumberFmt() == AmdGpu::NumberFormat::Float &&
           velocity.GetNumberFmt() == AmdGpu::NumberFormat::Float &&
           regs.color_target_mask.GetMask(0) == 0xf && regs.color_target_mask.GetMask(1) != 0 &&
           !regs.blend_control[0].enable && !regs.blend_control[1].enable &&
           !regs.depth_shader_control.mask_export_enable &&
           !regs.depth_shader_control.coverage_to_mask_enable;
}

inline bool Enabled() {
    static const bool enabled = [] {
        if (const char* value = std::getenv("RYGT_SMOOTH_GHOST")) {
            return value[0] == '1';
        }
        if (const auto value = Common::RytyGameConfigValue("smooth_ghost")) {
            return value->starts_with("true") || value->starts_with("1");
        }
        return true;
    }();
    return enabled;
}

inline bool ShouldSmooth(const AmdGpu::Regs& regs) {
    return IsGhostCoverageDraw(regs) && Enabled();
}

inline AmdGpu::BlendControl SmoothBlendControl() {
    AmdGpu::BlendControl blend{};
    blend.enable = 1;
    blend.color_src_factor = AmdGpu::BlendControl::BlendFactor::ConstantColor;
    blend.color_dst_factor = AmdGpu::BlendControl::BlendFactor::OneMinusConstantAlpha;
    blend.color_func = AmdGpu::BlendControl::BlendFunc::Add;
    blend.separate_alpha_blend = 1;
    blend.alpha_src_factor = AmdGpu::BlendControl::BlendFactor::One;
    blend.alpha_dst_factor = AmdGpu::BlendControl::BlendFactor::Zero;
    blend.alpha_func = AmdGpu::BlendControl::BlendFunc::Add;
    return blend;
}

inline constexpr AmdGpu::BlendConstants BlendConstants{0.25f, 0.25f, 0.25f, 0.25f};
inline constexpr u64 TireShaderHash = 0x0000000097fa55b7;
inline constexpr u64 RimShaderHash = 0x00000000531033a2;
inline constexpr u64 RimDetailShaderHash = 0x000000000bf717fe;
inline constexpr float RimOpacity = 0.25f;
inline constexpr float RimBrightness = 0.25f;
inline constexpr AmdGpu::BlendConstants RimBlendConstants{
    RimOpacity * RimBrightness, RimOpacity * RimBrightness, RimOpacity * RimBrightness, RimOpacity};
inline constexpr AmdGpu::BlendConstants RimDetailBlendConstants{
    RimOpacity, RimOpacity, RimOpacity, RimOpacity};

struct Appearance {
    int rubber_opacity = 25;
    int rim_opacity = 25;
    int rim_brightness = 25;
    int body_opacity = 25;
};

inline int ParsePercent(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return 25;
    }
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    int percent = 25;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), percent);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size()
               ? std::clamp(percent, 0, 100)
               : 25;
}

inline const Appearance& GetAppearance() {
    static const Appearance appearance = [] {
        const auto percent = [](const char* key) {
            const auto value = Common::RytyGameConfigValue(key);
            return value ? ParsePercent(*value) : 25;
        };
        return Appearance{percent("ghost_rubber_opacity"), percent("ghost_rim_opacity"),
                          percent("ghost_rim_brightness"), percent("ghost_body_opacity")};
    }();
    return appearance;
}

inline AmdGpu::BlendConstants OpacityConstants(int opacity, int brightness = 100) {
    const float alpha = std::clamp(opacity, 0, 100) / 100.0f;
    const float rgb = alpha * (std::clamp(brightness, 0, 100) / 100.0f);
    return {rgb, rgb, rgb, alpha};
}

inline AmdGpu::BlendConstants BlendConstantsForDraw(const AmdGpu::Regs& regs, u64 fragment_hash,
                                                   const Appearance& appearance) {
    if (!ShouldSmooth(regs)) {
        return regs.blend_constants;
    }
    if (fragment_hash == RimShaderHash) {
        return OpacityConstants(appearance.rim_opacity, appearance.rim_brightness);
    }
    if (fragment_hash == RimDetailShaderHash) {
        return OpacityConstants(appearance.rim_opacity);
    }
    if (fragment_hash == TireShaderHash) {
        return OpacityConstants(appearance.rubber_opacity);
    }
    return OpacityConstants(appearance.body_opacity);
}

inline AmdGpu::BlendConstants BlendConstantsForDraw(const AmdGpu::Regs& regs, u64 fragment_hash) {
    if (!ShouldSmooth(regs)) {
        return regs.blend_constants;
    }
    return BlendConstantsForDraw(regs, fragment_hash, GetAppearance());
}

} // namespace Vulkan::RytyGhost
