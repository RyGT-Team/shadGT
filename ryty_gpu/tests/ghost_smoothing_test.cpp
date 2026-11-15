#include "ryty_gpu/ryty_ghost.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace Vulkan::RytyGhost;
inline constexpr Appearance DefaultAppearance{};

void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main(int argc, char**) {
#ifdef _WIN32
    _putenv_s("SHADGT_SMOOTH_GHOST", argc > 1 ? "0" : "1");
#else
    setenv("SHADGT_SMOOTH_GHOST", argc > 1 ? "0" : "1", 1);
#endif
    auto regs = std::make_unique<AmdGpu::Regs>();
    std::memset(regs.get(), 0, sizeof(*regs));
    regs->reg_array[0xa30e] = 0xcccc9999;
    regs->reg_array[0xa30f] = 0x33336666;
    regs->color_control.mode = AmdGpu::ColorControl::OperationMode::Normal;
    regs->color_control.rop3 = AmdGpu::ColorControl::LogicOp::Copy;
    regs->depth_control.depth_enable = 1;
    regs->color_buffers[0].base_address = 0x10202000;
    regs->color_buffers[1].base_address = 0x10215f00;
    regs->color_buffers[0].info.format = u32(AmdGpu::DataFormat::Format10_11_11);
    regs->color_buffers[1].info.format = u32(AmdGpu::DataFormat::Format16_16);
    regs->color_buffers[0].info.number_type = u32(AmdGpu::NumberFormat::Float);
    regs->color_buffers[1].info.number_type = u32(AmdGpu::NumberFormat::Float);
    regs->color_target_mask.raw = 0xff;
    regs->blend_constants = {0.1f, 0.2f, 0.3f, 0.4f};
    Check(IsGhostCoverageDraw(*regs), "captured ghost state must match");
    if (argc > 1) {
        regs->primitive_type = AmdGpu::PrimitiveType::PatchPrimitive;
        for (const u64 hash : {TireShaderHash, RimShaderHash, RimDetailShaderHash}) {
            Check(BlendConstantsForDraw(*regs, hash, DefaultAppearance) == regs->blend_constants,
                  "disabled smoothing must preserve guest wheel blend constants");
        }
        std::cout << "PASS: disabled smoothing leaves rubber, metal rims and wheel details native\n";
        return 0;
    }
    std::swap(regs->reg_array[0xa30e], regs->reg_array[0xa30f]);
    Check(IsGhostCoverageDraw(*regs), "opposite temporal phase must match");
    const auto original = std::make_unique<AmdGpu::Regs>(*regs);
    unsigned rejected = 0;
    const auto reject = [&](auto change) {
        *regs = *original;
        change(*regs);
        Check(!IsGhostCoverageDraw(*regs), "unrelated or unsupported draw must stay native");
        for (const u64 hash : {TireShaderHash, RimShaderHash, RimDetailShaderHash}) {
            Check(BlendConstantsForDraw(*regs, hash, DefaultAppearance) == regs->blend_constants,
                  "every native-path guard must restore guest wheel blend constants");
        }
        ++rejected;
    };
    reject([](auto& r) {
        r.reg_array[0xa30e] = ~0u;
        r.reg_array[0xa30f] = ~0u;
    });
    reject([](auto& r) { r.reg_array[0xa30e] = r.reg_array[0xa30f] = 0; });
    reject([](auto& r) { r.reg_array[0xa30e] ^= 1; });
    reject([](auto& r) { r.depth_control.depth_enable = 0; });
    reject([](auto& r) { r.depth_buffer.z_info.num_samples = 1; });
    reject([](auto& r) { r.color_buffers[0].attrib.num_fragments_log2 = 1; });
    reject([](auto& r) { r.color_buffers[1].attrib.num_fragments_log2 = 1; });
    reject([](auto& r) { r.color_buffers[0].base_address = 0; });
    reject([](auto& r) { r.color_buffers[1].base_address = 0; });
    reject(
        [](auto& r) { r.color_buffers[0].info.format = u32(AmdGpu::DataFormat::Format8_8_8_8); });
    reject([](auto& r) { r.color_buffers[1].info.format = u32(AmdGpu::DataFormat::Format32); });
    reject([](auto& r) { r.color_buffers[0].info.number_type = u32(AmdGpu::NumberFormat::Unorm); });
    reject([](auto& r) { r.color_buffers[1].info.number_type = u32(AmdGpu::NumberFormat::Unorm); });
    reject([](auto& r) { r.color_target_mask.raw = 0xf; });
    reject([](auto& r) { r.color_target_mask.raw = 0xf1; });
    reject([](auto& r) { r.blend_control[0].enable = 1; });
    reject([](auto& r) { r.blend_control[1].enable = 1; });
    reject([](auto& r) { r.depth_shader_control.mask_export_enable = 1; });
    reject([](auto& r) { r.depth_shader_control.coverage_to_mask_enable = 1; });
    reject([](auto& r) { r.color_control.mode = AmdGpu::ColorControl::OperationMode::Disable; });
    reject([](auto& r) { r.color_control.rop3 = AmdGpu::ColorControl::LogicOp::Xor; });
    *regs = *original;
    Check(IsGhostCoverageDraw(*regs) && std::memcmp(regs.get(), original.get(), sizeof(*regs)) == 0,
          "selection must not modify guest registers");
    regs->primitive_type = AmdGpu::PrimitiveType::PatchPrimitive;
    const auto wheel_state = std::make_unique<AmdGpu::Regs>(*regs);
    const Appearance custom{75, 20, 40, 10};
    Check(BlendConstantsForDraw(*regs, TireShaderHash, custom) ==
              AmdGpu::BlendConstants{0.75f, 0.75f, 0.75f, 0.75f},
          "configured rubber opacity must be independent of body opacity");
    Check(BlendConstantsForDraw(*regs, 0x144a287b, custom) ==
              AmdGpu::BlendConstants{0.1f, 0.1f, 0.1f, 0.1f},
          "configured body opacity must be independent of rubber opacity");
    const auto custom_rim = BlendConstantsForDraw(*regs, RimShaderHash, custom);
    Check(std::abs(custom_rim[0] - 0.08f) < 0.000001f && custom_rim[3] == 0.2f,
          "configured rim brightness must multiply its source weight independently of opacity");
    Check(BlendConstantsForDraw(*regs, RimDetailShaderHash, custom) ==
              AmdGpu::BlendConstants{0.2f, 0.2f, 0.2f, 0.2f},
          "wheel details must follow rim opacity without extra brightness scaling");
    Check(ParsePercent(" 75 \t") == 75 && ParsePercent("0") == 0 &&
              ParsePercent("-10") == 0 && ParsePercent("101") == 100 &&
              ParsePercent("") == 25 && ParsePercent("true") == 25 &&
              ParsePercent("\"50\"") == 25 && ParsePercent("50junk") == 25 &&
              ParsePercent("25.5") == 25 && ParsePercent("99999999999999") == 25,
          "JSON percentages must clamp valid integers and default malformed values safely");
    Check(OpacityConstants(0) == AmdGpu::BlendConstants{0, 0, 0, 0} &&
              OpacityConstants(100) == AmdGpu::BlendConstants{1, 1, 1, 1} &&
              OpacityConstants(100, 0) == AmdGpu::BlendConstants{0, 0, 0, 1},
          "opacity and brightness endpoints must retain independent destination weights");
    Check(BlendConstantsForDraw(*regs, TireShaderHash, DefaultAppearance) == BlendConstants,
          "rubber must use the same quarter-opacity blend as the body");
    Check(BlendConstants == AmdGpu::BlendConstants{0.25f, 0.25f, 0.25f, 0.25f},
          "body and rubber RGB and destination weights must preserve quarter opacity");
    Check(BlendConstantsForDraw(*regs, RimShaderHash, DefaultAppearance) == RimBlendConstants &&
              RimBlendConstants == AmdGpu::BlendConstants{0.0625f, 0.0625f, 0.0625f, 0.25f},
          "metal rims must combine quarter opacity and quarter brightness");
    Check(BlendConstantsForDraw(*regs, RimDetailShaderHash, DefaultAppearance) == RimDetailBlendConstants &&
              RimDetailBlendConstants == AmdGpu::BlendConstants{0.25f, 0.25f, 0.25f, 0.25f},
          "wheel detail opacity must use quarter opacity without dimming its shader output");
    Check(BlendConstantsForDraw(*regs, 0x144a287b, DefaultAppearance) == BlendConstants &&
              BlendConstantsForDraw(*regs, 0x10000000531033a2, DefaultAppearance) == BlendConstants &&
              BlendConstantsForDraw(*regs, 0x100000000bf717fe, DefaultAppearance) == BlendConstants &&
              BlendConstantsForDraw(*regs, 0, DefaultAppearance) == BlendConstants,
          "body, unrelated and absent fragment shaders must keep the requested quarter opacity");
    Check(std::memcmp(regs.get(), wheel_state.get(), sizeof(*regs)) == 0,
          "wheel adjustment must not change depth, stencil or any other guest state");
    regs->primitive_type = AmdGpu::PrimitiveType::TriangleList;
    Check(BlendConstantsForDraw(*regs, TireShaderHash, DefaultAppearance) == BlendConstants,
          "rubber must remain quarter opaque without a primitive-specific override");
    regs->primitive_type = AmdGpu::PrimitiveType::PatchPrimitive;
    regs->reg_array[0xa30e] = regs->reg_array[0xa30f] = ~0u;
    for (const u64 hash : {TireShaderHash, RimShaderHash, RimDetailShaderHash}) {
        Check(BlendConstantsForDraw(*regs, hash, DefaultAppearance) == regs->blend_constants,
              "ordinary cars using the same wheel shaders must stay native");
    }
    *regs = *original;
    for (const auto phase :
         {std::array<u32, 2>{0xcccc9999, 0x33336666}, std::array<u32, 2>{0x33336666, 0xcccc9999}}) {
        for (const unsigned samples : {1u, 2u, 4u, 8u, 16u}) {
            unsigned covered = 0;
            for (unsigned y = 0; y < 2; ++y)
                for (unsigned x = 0; x < 2; ++x)
                    covered += std::popcount((phase[y] >> (16 * x)) & ((1u << samples) - 1));
            Check(float(covered) / (4 * samples) == 0.5f,
                  "native coverage must remain half while the requested visual opacity is quarter");
        }
    }
    const auto blend = SmoothBlendControl();
    Check(blend.enable && blend.color_func == AmdGpu::BlendControl::BlendFunc::Add &&
              blend.color_src_factor == AmdGpu::BlendControl::BlendFactor::ConstantColor &&
              blend.color_dst_factor == AmdGpu::BlendControl::BlendFactor::OneMinusConstantAlpha,
          "RGB blending must mix the ghost and background independently of source alpha");
    Check(blend.separate_alpha_blend &&
              blend.alpha_src_factor == AmdGpu::BlendControl::BlendFactor::One &&
              blend.alpha_dst_factor == AmdGpu::BlendControl::BlendFactor::Zero,
          "original alpha export must be preserved");
    for (const auto source : {0.0f, 0.03125f, 0.5f, 1.0f, 2.1640625f, 16.0f, 10000.0f}) {
        for (const auto destination : {0.0f, 0.06640625f, 0.5f, 1.0f, 4.0f}) {
            const auto compare = [&](const AmdGpu::BlendConstants& constants, float opacity,
                                     float brightness) {
                for (unsigned channel = 0; channel < 3; ++channel) {
                    const float actual = source * constants[channel] +
                                         destination * (1.0f - constants[3]);
                    const float preview = (source * brightness) * opacity +
                                          destination * (1.0f - opacity);
                    Check(std::abs(actual - preview) <= 0.000001f * (1.0f + std::abs(preview)),
                          "blend-state output must reproduce preview opacity and brightness independently");
                }
            };
            compare(BlendConstants, 0.25f, 1.0f);
            compare(RimBlendConstants, 0.25f, 0.25f);
            compare(RimDetailBlendConstants, 0.25f, 1.0f);
        }
    }
    std::cout << "PASS: captured ghost format and phases; " << rejected
              << " native-path guards; immutable guest state; mean coverage and blending; "
                 "JSON settings, rubber/body/rim isolation, independent opacity/brightness, and native-car restoration\n";
}
