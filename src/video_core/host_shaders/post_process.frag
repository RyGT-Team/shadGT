// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450
#extension GL_GOOGLE_include_directive : require

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;
layout (binding = 1) uniform sampler2D uiSampler; // ryGT HDR-UI: the UI layer
// GT7-UW: the HUD drawn over black and over white, laid over the picture at 16:9.
layout (binding = 2) uniform sampler2D hudBlack;
layout (binding = 3) uniform sampler2D hudWhite;
// GT7-UW: HUD pieces placed elsewhere: src (x, y, w, h of the 16:9 HUD), dst (of the output).
struct HudWidget {
    vec4 src;
    vec4 dst;
};
layout (std140, binding = 4) uniform HudLayout {
    uvec4 count;
    HudWidget widgets[16];
} layout_;

layout (push_constant) uniform settings {
    // Must match PostProcessingConstants (PostProcessingPass::Settings first) in pp_pass.cpp.
    // Flags are uint (u32 on the C++ side).
    float gamma;
    uint hdr;
    uint srgb_input;
    float hdr_peak_nits;
    float hdr_gain;
    uint sdr_input;
    float sdr_white_nits;
    uint hdr_input_709;
    uint ui_layer;
    float ui_white_nits;
    float hdr_saturation;
    // GT7-UW: the HUD laid over the picture between hud_x0 and hud_x1 of the width.
    uint hud;
    float hud_x0;
    float hud_x1;
    float dlss_sharpening;
    // ryGT WIDE-042: the ambient sides (outside side_x0..side_x1 of the width) dimmed by side_dim.
    float side_x0;
    float side_x1;
    float side_dim;
} pp;

#define A_GPU 1
#define A_GLSL 1
#include "fsr/ffx_a.h"
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) {
    return texelFetch(texSampler, clamp(p, ivec2(0), textureSize(texSampler, 0) - 1), 0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) { }
#include "fsr/ffx_fsr1.h"

// ryGT: SMPTE ST 2084 (PQ) and the BT.2390 roll-off of highlights above the display's peak.
const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
float pq_to_nits(float e) {
    float p = pow(max(e, 0.0), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}
float nits_to_pq(float n) {
    float y = pow(clamp(n, 0.0, 10000.0) / 10000.0, m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}
float eetf(float e, float max_lum) {
    max_lum = min(max_lum, 0.95); // keeps the knee below 1 (a peak of 10000 divided by zero)
    e = clamp(e, 0.0, 1.0);
    float ks = 1.5 * max_lum - 0.5;
    if (e <= ks) {
        return e;
    }
    float t = (e - ks) / (1.0 - ks);
    float t2 = t * t, t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) +
           (-2.0 * t3 + 3.0 * t2) * max_lum;
}
const mat3 bt709_to_bt2020 = mat3(0.6274, 0.0691, 0.0164,
                                  0.3293, 0.9195, 0.0880,
                                  0.0433, 0.0114, 0.8956);
vec3 hdr_adjust(vec3 pq) {
    // FIX-GT7-255: nothing to adjust (GT7's default): the game's codes as they are. Through nits and
    // back, the dark channels of saturated colours moved by up to 2 of 1023 codes.
    if (pp.hdr_peak_nits <= 0.0 && pp.hdr_gain == 1.0 && pp.hdr_saturation == 1.0 &&
        pp.hdr_input_709 == 0u) {
        return pq;
    }
    vec3 nits = vec3(pq_to_nits(pq.r), pq_to_nits(pq.g), pq_to_nits(pq.b)) * pp.hdr_gain;
    // Saturation around the BT.2020 luminance.
    float lum = dot(nits, vec3(0.2627, 0.6780, 0.0593));
    nits = max(vec3(lum) + (nits - vec3(lum)) * pp.hdr_saturation, vec3(0.0));
    if (pp.hdr_input_709 != 0u) {
        nits = max(bt709_to_bt2020 * nits, vec3(0.0));
    }
    if (pp.hdr_peak_nits <= 0.0) {
        return vec3(nits_to_pq(nits.r), nits_to_pq(nits.g), nits_to_pq(nits.b));
    }
    // On the brightest channel, so the hue is kept.
    float peak = max(max(nits.r, nits.g), nits.b);
    if (peak <= 0.0) {
        return vec3(0.0);
    }
    float e = nits_to_pq(peak);
    float mapped = pq_to_nits(eetf(e, nits_to_pq(pp.hdr_peak_nits)));
    nits *= mapped / peak;
    return vec3(nits_to_pq(nits.r), nits_to_pq(nits.g), nits_to_pq(nits.b));
}

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

bool inside(vec2 p, vec4 rect) {
    return p.x >= rect.x && p.y >= rect.y && p.x < rect.x + rect.z && p.y < rect.y + rect.w;
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    if (pp.dlss_sharpening > 0.0) {
        ivec2 pixel = clamp(ivec2(uv * textureSize(texSampler, 0)), ivec2(0),
                            textureSize(texSampler, 0) - 1);
        vec3 sharpened;
        FsrRcasF(sharpened.r, sharpened.g, sharpened.b, uvec2(pixel),
                 uvec4(floatBitsToUint(pp.dlss_sharpening), 0u, 0u, 0u));
        color_linear.rgb = sharpened;
    }
    if (pp.side_dim < 1.0 && (uv.x < pp.side_x0 || uv.x > pp.side_x1)) {
        color_linear.rgb *= pp.side_dim;
    }
    if (pp.hud != 0u) {
        // Where on the 16:9 HUD this pixel shows: a placed piece's, else the centred HUD's
        // (but not what a piece shows elsewhere).
        bool shown = false;
        vec2 hud_uv = vec2(0.0);
        const uint count = min(layout_.count.x, 16u);
        for (uint i = 0u; i < count; ++i) {
            const HudWidget w = layout_.widgets[i];
            if (inside(uv, w.dst)) {
                hud_uv = w.src.xy + (uv - w.dst.xy) / w.dst.zw * w.src.zw;
                shown = true;
                break;
            }
        }
        if (!shown && uv.x >= pp.hud_x0 && uv.x <= pp.hud_x1) {
            hud_uv = vec2((uv.x - pp.hud_x0) / (pp.hud_x1 - pp.hud_x0), uv.y);
            shown = true;
            for (uint i = 0u; i < count; ++i) {
                if (inside(hud_uv, layout_.widgets[i].src)) {
                    shown = false;
                    break;
                }
            }
        }
        if (shown) {
            // Over black the HUD left its colour times its coverage; over white also
            // 1 - coverage. Blended as the game blends it, in the display buffer's encoding.
            const vec3 black = texture(hudBlack, hud_uv).rgb;
            const vec3 white = texture(hudWhite, hud_uv).rgb;
            const float coverage = clamp(1.0 - dot(white - black, vec3(1.0 / 3.0)), 0.0, 1.0);
            color_linear.rgb = color_linear.rgb * (1.0 - coverage) + black;
        }
    }
    if ((pp.hdr != 0u) && (pp.sdr_input != 0u)) {
        // SDR frame on the HDR10 swapchain: BT.709 linear to BT.2020, at the SDR white level.
        vec3 lin = (pp.srgb_input != 0u) ? degamma(color_linear.rgb) : color_linear.rgb;
        vec3 nits = max(bt709_to_bt2020 * lin, vec3(0.0)) * pp.sdr_white_nits;
        color = vec4(nits_to_pq(nits.r), nits_to_pq(nits.g), nits_to_pq(nits.b), color_linear.a);
    } else if ((pp.hdr != 0u) && (pp.ui_layer != 0u)) {
        // HDR-UI: the scene (PQ) with the SDR UI layer over it, composed in linear light.
        vec3 scene = hdr_adjust(color_linear.rgb);
        vec3 scene_nits = vec3(pq_to_nits(scene.r), pq_to_nits(scene.g), pq_to_nits(scene.b));
        vec4 ui = texture(uiSampler, uv);
        float a = clamp(ui.a, 0.0, 1.0);
        vec3 ui_lin = a > 0.0 ? degamma(clamp(ui.rgb / a, 0.0, 1.0)) * a : vec3(0.0);
        vec3 nits = scene_nits * (1.0 - a) + max(bt709_to_bt2020 * ui_lin, vec3(0.0)) * pp.ui_white_nits;
        color = vec4(nits_to_pq(nits.r), nits_to_pq(nits.g), nits_to_pq(nits.b), 1.0);
    } else if (pp.hdr != 0u) {
        color = vec4(hdr_adjust(color_linear.rgb), color_linear.a);
    } else {
        if (pp.srgb_input != 0u) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
