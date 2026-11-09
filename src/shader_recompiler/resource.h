// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdlib>
#include "common/types.h"
#include "shader_recompiler/ir/type.h"
#include "video_core/amdgpu/resource.h"

#include <boost/container/static_vector.hpp>

namespace Shader {

static constexpr u32 NUM_USER_DATA_REGS = 16;
static constexpr u32 NUM_IMAGES = 64;
static constexpr u32 NUM_BUFFERS = 40;
static constexpr u32 NUM_SAMPLERS = 16;
// FIX-GT7-043: every read of a T# that looks like an FMask takes a place, not only size queries: one
// race launch had up to 6 in a program, and a list that overflows ends the process.
static constexpr u32 NUM_FMASKS = 24;

using SharpLocation = u16;

struct SharpFetch {
    enum class Summary : u8 {
        Id,
        Min,
        Max,
        Avg,
    };
    u32 offset;
    u32 count;
    Summary summary;
    u32 src_comp;
    u32 dst_start;
    constexpr bool operator==(const SharpFetch&) const = default;
};

struct SharpFetchPostOp : u8 {
    NativeCubeSampling,
    Normal,
};

enum class BufferType : u8 {
    Unknown = 0,
    U8,
    U16,
    U32,
    S8,
    S16,
    S32,
    F16,
    F32,
    F64,
    U64,
    U128,
};

struct BufferResource {
    BufferType type;
    u32 offset;
    u32 size;
    u32 stride;
    u32 num_elements;
    u32 offset_reg;
    u32 size_reg;
    u32 stride_reg;
    u32 num_elements_reg;
};

using BufferResourceList = boost::container::static_vector<BufferResource, NUM_BUFFERS>;

struct ImageResource {
    enum class Dim : u8 {
        Unknown = 0,
        _1D = 1,
        _2D = 2,
        _3D = 3,
        Cube = 4,
        _1DArray = 5,
        _2DArray = 6,
        CubeArray = 7,
        Buffer = 8,
        _2DMultisample = 9,
        _2DMultisampleArray = 10,
    };
    Dim dim;
    bool is_array;
    bool is_multisample;
    bool is_cube;
    u32 format;
    u32 num_samples;
    SharpFetch sharp_fetch;
    SharpFetchPostOp post_op;
    u32 texel_size;
    bool is_integer;
    bool is_depth;
    bool is_cube;

    constexpr AmdGpu::ImageType GetViewType(bool is_array) const noexcept {
        if (dim == Dim::_1D || dim == Dim::_1DArray) {
            return is_array ? AmdGpu::ImageType::Color1DArray : AmdGpu::ImageType::Color1D;
        }
        if (dim == Dim::_2D || dim == Dim::_2DArray || dim == Dim::_2DMultisample || dim == Dim::_2DMultisampleArray) {
            return is_array ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
        }
        if (dim == Dim::_3D) {
            return AmdGpu::ImageType::Color3D;
        }
        if (dim == Dim::Cube || dim == Dim::CubeArray) {
            return AmdGpu::ImageType::Cube;
        }
        return AmdGpu::ImageType::Invalid;
    }
};

using ImageResourceList = boost::container::static_vector<ImageResource, NUM_IMAGES>;

struct SamplerResource {
    u32 filter;
    u32 wrap_s;
    u32 wrap_t;
    u32 wrap_r;
    u32 min_filter;
    u32 mag_filter;
    u32 mip_filter;
    u32 min_lod;
    u32 max_lod;
    u32 lod_bias;
    u32 compare_mode;
    u32 compare_func;
    u32 border_color;
    u32 aniso_ratio;
    u32 aniso_threshold;

    constexpr AmdGpu::Sampler GetSharp(const auto& info) const noexcept {
        AmdGpu::Sampler sampler{};
        sampler.filter = static_cast<AmdGpu::Sampler::Filter>(filter);
        sampler.wrap_s = static_cast<AmdGpu::Sampler::Wrap>(wrap_s);
        sampler.wrap_t = static_cast<AmdGpu::Sampler::Wrap>(wrap_t);
        sampler.wrap_r = static_cast<AmdGpu::Sampler::Wrap>(wrap_r);
        sampler.min_filter = static_cast<AmdGpu::Sampler::Filter>(min_filter);
        sampler.mag_filter = static_cast<AmdGpu::Sampler::Filter>(mag_filter);
        sampler.mip_filter = static_cast<AmdGpu::Sampler::Filter>(mip_filter);
        sampler.min_lod = static_cast<AmdGpu::Sampler::Lod>(min_lod);
        sampler.max_lod = static_cast<AmdGpu::Sampler::Lod>(max_lod);
        sampler.lod_bias = lod_bias;
        sampler.compare_mode = compare_mode;
        sampler.compare_func = compare_func;
        sampler.border_color = border_color;
        sampler.aniso_ratio = aniso_ratio;
        sampler.aniso_threshold = aniso_threshold;
        return sampler;
    }
};
using SamplerResourceList = boost::container::static_vector<SamplerResource, NUM_SAMPLERS>;

struct FMaskResource {
    SharpFetch<AmdGpu::Image> sharp_fetch{};
    bool is_r128{};
    bool size_read{};

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        AmdGpu::Image image{};
        const bool fetched = is_r128 ? sharp_fetch.Fetch<4>(info.flattened_ud_buf.data(), &image)
                                     : sharp_fetch.Fetch(info.flattened_ud_buf.data(), &image);
        return fetched ? image : AmdGpu::Image::Null(false);
    }
};
using FMaskResourceList = boost::container::static_vector<FMaskResource, NUM_FMASKS>;

struct PushData {
    static constexpr u32 XOffsetIndex = 0;
    static constexpr u32 YOffsetIndex = 1;
    static constexpr u32 XScaleIndex = 2;
    static constexpr u32 YScaleIndex = 3;
    static constexpr u32 BufOffsetIndex = 4;
    /// After the six buffer-offset vectors.
    static constexpr u32 QuadSampleMaskIndex = 10;
    /// FIX-227: scratch address aliases the all-ones sample mask on eligible draws.
    static constexpr u32 LdsScratchIndex = QuadSampleMaskIndex;

    float xoffset;
    float yoffset;
    float xscale;
    float yscale;
    /// 96 offsets, each stage with its own range (Backend::StageBufferOffsetBase).
    std::array<u8, 96> buf_offsets;
    std::array<u32, 2> quad_sample_mask{~0u, ~0u};

    void AddOffset(u32 binding, u32 offset) {
        ASSERT(offset < 256 && binding < buf_offsets.size());
        buf_offsets[binding] = offset;
    }
};
static_assert(offsetof(PushData, quad_sample_mask) == 112 && sizeof(PushData) == 120);
static_assert(sizeof(PushData) <= 128,
              "PushData size is greater than minimum size guaranteed by Vulkan spec");

/// FIX-GT7-125: an MSAA array T# (Color2DMsaaArray, kept by array instructions) is translated:
/// samples and loads read it as a multisampled array, its size is queried without a level. The
/// translation stopped at the image type before (no SPIR-V type, no sample coordinates for it),
/// and GT7's draws with such a T# were queued again and again and never drawn (one failed
/// translation every 2 s in quiet race stretches of 11-08). RYGT_MSAA_ARRAY_SAMPLE=0: as before.
inline bool MsaaArrayEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("RYGT_MSAA_ARRAY_SAMPLE");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

/// FIX-GT7-125: the multisampled view types the translation handles.
inline bool IsMsaaView(AmdGpu::ImageType type) {
    return type == AmdGpu::ImageType::Color2DMsaa ||
           (type == AmdGpu::ImageType::Color2DMsaaArray && MsaaArrayEnabled());
}

enum class MipStorageFallbackMode : u16 {
    None,
    DynamicIndex,
    ConstantIndex,
};

} // namespace Shader
