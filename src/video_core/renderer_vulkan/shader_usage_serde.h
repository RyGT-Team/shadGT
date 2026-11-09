// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>

#include "common/serdes.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/specialization.h"

namespace Vulkan {

inline constexpr u32 ClassicCubeTag = 0x80000000u;
constexpr u32 CubeTaggedVersion(u32 version, bool face_cubes) {
    return version | (face_cubes ? ClassicCubeTag : 0u);
}
constexpr u32 UntagCubeVersion(u32 version) { return version & ~ClassicCubeTag; }
constexpr size_t NextUnusedStoredSlot(size_t previous_end, size_t occupied_index) {
    return std::max(previous_end, occupied_index + 1);
}
template <typename Occupied>
size_t FindUnusedStoredSlot(size_t candidate, Occupied&& occupied) {
    while (occupied(candidate)) ++candidate;
    return candidate;
}
constexpr u32 FragmentBinaryVersion(bool discard_kill, bool single_sample_barycentric,
                                    bool native_cube, u32 discard_version,
                                    u32 barycentric_version, u32 native_cube_version) {
    return discard_kill ? discard_version : single_sample_barycentric ? barycentric_version
                                                 : native_cube ? native_cube_version : 0u;
}

inline bool TranslatedWithFaceCubes(const Shader::StageSpecialization& spec, bool native_cube) {
    return !native_cube && spec.runtime_info.hw_stage == Shader::HwStage::Fragment &&
           std::ranges::any_of(spec.images, [](const Shader::ImageSpecialization& image) {
               return image.type == AmdGpu::ImageType::Cube && !image.is_storage;
           });
}
inline u32 CubeVersionFor(u32 base_version, const Shader::StageSpecialization& spec, bool native_cube) {
    return CubeTaggedVersion(base_version, TranslatedWithFaceCubes(spec, native_cube));
}
inline bool StoredCubesFit(const Shader::StageSpecialization& spec, const Shader::Info& info,
                           bool face_cubes, bool native_cube) {
    if (spec.runtime_info.hw_stage != Shader::HwStage::Fragment) return true;
    const bool sampled_cube = std::ranges::any_of(spec.images, [](const auto& image) {
        return image.type == AmdGpu::ImageType::Cube && !image.is_storage;
    });
    const bool translated_native = std::ranges::any_of(info.images, [](const auto& image) {
        return image.post_op == Shader::SharpFetchPostOp::NativeCubeSampling;
    });
    return native_cube ? (!face_cubes && (!sampled_cube || translated_native)) : !translated_native;
}

/// FIX-043's record of which bound-resource properties a translation read, stored after the
/// shader's Info from meta version 16. Without it a stored permutation, whose key leaves those
/// properties out, would never match the key rebuilt from the loaded shader.
inline void WriteResourceUsage(Serialization::Writer& out, const Shader::Info& info) {
    out.Write(u8{info.resource_usage_known});
    out.Write(u64{info.buffer_stride_used.to_ullong()});
    out.Write(u64{info.image_srgb_used.to_ullong()});
}

inline void ReadResourceUsage(Serialization::Reader& in, Shader::Info& info) {
    u8 known{};
    u64 buffers{};
    u64 images{};
    in.Read(known);
    in.Read(buffers);
    in.Read(images);
    info.resource_usage_known = known != 0;
    info.buffer_stride_used = std::bitset<Shader::NUM_BUFFERS>{buffers};
    info.image_srgb_used = std::bitset<Shader::NUM_IMAGES>{images};
}

} // namespace Vulkan
