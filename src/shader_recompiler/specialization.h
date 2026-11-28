// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bitset>
#include <span>
#include <type_traits>

#include "common/perf_monitor.h"
#include "common/types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"

namespace Shader {

struct VsAttribSpecialization {
    u32 divisor{};
    AmdGpu::NumberClass num_class{};
    AmdGpu::CompMapping dst_select{};

    bool operator==(const VsAttribSpecialization&) const = default;
};

struct BufferSpecialization {
    u32 stride : 14;
    u32 is_formatted : 1;
    u32 swizzle_enable : 1;
    u32 data_format : 6;
    u32 num_format : 4;
    u32 index_stride : 2;
    u32 element_size : 2;
    // FIX-043: recorded only for a translation with known resource usage (zero otherwise).
    u32 add_tid_enable : 1;
    u32 coherent : 1;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};

    bool operator==(const BufferSpecialization& other) const {
        return stride == other.stride && is_formatted == other.is_formatted &&
               swizzle_enable == other.swizzle_enable && add_tid_enable == other.add_tid_enable &&
               coherent == other.coherent &&
               (!is_formatted ||
                (data_format == other.data_format && num_format == other.num_format &&
                 dst_select == other.dst_select && num_conversion == other.num_conversion)) &&
               (!swizzle_enable ||
                (index_stride == other.index_stride && element_size == other.element_size));
    }

    /// FIX-GT7-024: equal but for addressing (stride, swizzling, thread-id addressing and
    /// coherence), which reads of a buffer bound empty don't depend on.
    bool SameButAddressing(const BufferSpecialization& other) const {
        return is_formatted == other.is_formatted &&
               (!is_formatted ||
                (data_format == other.data_format && num_format == other.num_format &&
                 dst_select == other.dst_select && num_conversion == other.num_conversion));
    }
};

struct ImageSpecialization {
    AmdGpu::ImageType type = AmdGpu::ImageType::Color2D;
    bool is_integer = false;
    bool is_storage = false;
    bool is_srgb = false;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};
    // FIXME any pipeline cache changes needed?
    u32 num_bindings = 0;

    bool operator==(const ImageSpecialization&) const = default;
};

struct FMaskSpecialization {
    u32 width;
    u32 height;

    bool operator==(const FMaskSpecialization&) const = default;
};

struct SamplerSpecialization {
    u8 force_unnormalized : 1;
    u8 force_degamma : 1;

    bool operator==(const SamplerSpecialization&) const = default;
};

/**
 * Alongside runtime information, this structure also checks bound resources
 * for compatibility. Can be used as a key for storing shader permutations.
 * Is separate from runtime information, because resource layout can only be deduced
 * after the first compilation of a module.
 */
struct StageSpecialization {
    static constexpr size_t MaxStageResources = 128;
    static_assert(NUM_BUFFERS + NUM_IMAGES + NUM_FMASKS <= MaxStageResources);

    const Info* info{};
    RuntimeInfo runtime_info{};
    std::bitset<MaxStageResources> bitset{};
    /// FIX-GT7-024: bound buffers the rasterizer binds empty (IsBoundEmpty). Not stored: only
    /// a lookup's own (the right side of a comparison) is read.
    std::bitset<MaxStageResources> bound_empty{};
    Gcn::FetchShaderData fetch_shader_data{};
    SmallVector<VsAttribSpecialization, 32> vs_attribs;
    SmallVector<BufferSpecialization, 16> buffers;
    SmallVector<ImageSpecialization, 16> images;
    SmallVector<FMaskSpecialization, 8> fmasks;
    SmallVector<SamplerSpecialization, 16> samplers;
    Backend::Bindings start{};

    StageSpecialization() = default;
    StageSpecialization(const Info& info_, RuntimeInfo runtime_info_, const Profile& profile_,
                        Backend::Bindings start_,
                        const Gcn::FetchShaderData* translated_fetch = nullptr,
                        const RuntimeInfo* compiled_tess_layout = nullptr)
        : info{&info_}, runtime_info{runtime_info_}, start{start_} {
        bool parsed_fetch = false;
        if (info_.sw_stage == SwStage::Vertex) {
            if (translated_fetch) {
                fetch_shader_data = *translated_fetch;
                parsed_fetch = true;
            } else {
                parsed_fetch = Gcn::ParseFetchShader(info_, fetch_shader_data);
            }
        }
        if (parsed_fetch) {
            // Specialize shader on VS input number types to follow spec.
            ForEachSharp(vs_attribs, fetch_shader_data.attributes,
                         [this](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                             SetVertexAttributeSpec(spec, desc, sharp, runtime_info);
                         });
        }
        // FIX-043: with the resource usage of the translation known, the key holds only the
        // properties the code was built from: a buffer's stride when it entered an address, an
        // image's sRGB format when it decided a forced degamma, and for an unbound buffer the
        // V# translation saw instead of zeros. Thread-id addressing and coherence, which the
        // code also reads from the V#, are added to the key.
        const bool usage_known = info->resource_usage_known;
        u32 binding{};
        ForEachSharp(
            binding, buffers, info->buffers,
            [this, &binding](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                const size_t index = &desc - info->buffers.data();
                if (BoundEmptyForMatch(desc, sharp)) {
                    bound_empty[binding - 1] = true; // ForEachSharp counted it already
                }
                // FIX-SGT-008: a V# whose last word has reserved bits or a type set is no buffer (an unused slot holding
                // leftover floats, e.g. 44441916 00000000 c454de6c 3f800000 in GT7's car shader
                // 0xc4fac70): its words changed from draw to draw and made a new permutation each
                // time, so GT7 translated and compiled the same shaders again all session and their
                // draws were skipped (endless popping). Such a slot is specialized as a default
                // buffer. SHADGT_BUFFER_JUNK_FILTER=0: as before.
                // Only the last word's reserved bits and type count: GT7 keeps other data in the
                // address's upper byte (_padding0) of buffers it really reads (fs 0x7e5778be:
                // 11288000 00407010 00000001 18027204), which the hardware ignores.
                SetBufferSpec(spec, *info, index, sharp);
            },
            usage_known);
        ForEachSharp(
            binding, images, info->images, [&](auto& spec, const auto& desc, AmdGpu::Image sharp) {
                // FIX-GT7-043: translation has no image for an FMask, so code with one isn't for
                // this T#. Two translations matching one state would let the read-ahead change the
                // current draw's key, as the last match decides between them.
                spec.type = FMaskKeyEnabled() && sharp.Valid() && AmdGpu::IsFmask(sharp.GetDataFmt())
                                ? AmdGpu::ImageType::Invalid
                                : sharp.GetViewType(desc.is_array);
                spec.is_integer = AmdGpu::IsInteger(sharp.GetNumberFmt());
                spec.is_storage = desc.is_written;
                if (spec.is_storage) {
                    spec.dst_select = sharp.DstSelect();
                } else if (!usage_known || info->image_srgb_used[&desc - info->images.data()]) {
                    spec.is_srgb = sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb;
                }
                spec.num_conversion = sharp.GetNumberConversion();
                spec.num_bindings = desc.NumBindings(*info);
            },
            // SHD-003: an empty slot records the null image translation saw (a 2D float image),
            // so a texture bound there later that translates the same way matches instead of
            // getting a new translation (one in 16 pixel shader translations was such a copy).
            true);
        ForEachSharp(binding, fmasks, info->fmasks,
                     [](auto& spec, const auto& desc, AmdGpu::Image sharp) {
                         if (!FMaskKeyEnabled()) {
                             spec.width = sharp.width;
                             spec.height = sharp.height;
                             return;
                         }
                         // FIX-GT7-043: zero for a T# that isn't an FMask, which this code
                         // can't read. The size counts only where the code reads it: GT7 leaves
                         // float data in many slots, which would make a variant per value.
                         if (sharp.Valid() && AmdGpu::IsFmask(sharp.GetDataFmt())) {
                             spec.width = desc.size_read ? sharp.width + 1 : 1;
                             spec.height = desc.size_read ? sharp.height + 1 : 1;
                         }
                     });
        ForEachSharp(samplers, info->samplers,
                     [](auto& spec, const auto& desc, AmdGpu::Sampler sharp) {
                         spec.force_unnormalized = sharp.force_unnormalized;
                         spec.force_degamma = sharp.force_degamma;
                     });

        // Initialize runtime_info fields that rely on analysis in tessellation passes
        if (info->sw_stage == SwStage::TessellationControl ||
            info->sw_stage == SwStage::TessellationEval) {
            if (compiled_tess_layout) {
                if (info->sw_stage == SwStage::TessellationControl) {
                    runtime_info.sw.tcs.ls_stride = compiled_tess_layout->sw.tcs.ls_stride;
                    runtime_info.sw.tcs.hs_output_cp_stride = compiled_tess_layout->sw.tcs.hs_output_cp_stride;
                    runtime_info.sw.tcs.hs_output_base = compiled_tess_layout->sw.tcs.hs_output_base;
                } else {
                    runtime_info.sw.tes.hs_output_cp_stride = compiled_tess_layout->sw.tes.hs_output_cp_stride;
                }
                return;
            }
            TessellationDataConstantBuffer tess_constants{};
            info->ReadTessConstantBuffer(tess_constants);
            runtime_info.InitFromTessConstants(tess_constants);
        }
    }

    static void SetVertexAttributeSpec(VsAttribSpecialization& spec, const Gcn::VertexAttribute& desc,
                                       AmdGpu::Buffer sharp, const RuntimeInfo& runtime_info) {
        using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
        if (const auto step_rate = desc.GetStepRate();
            step_rate != InstanceIdType::None) {
            spec.divisor = step_rate == InstanceIdType::OverStepRate0
                               ? runtime_info.sw.vs.step_rate_0
                               : (step_rate == InstanceIdType::OverStepRate1
                                      ? runtime_info.sw.vs.step_rate_1
                                      : 1);
        }
        // FIX-060: a V# of a gigabyte or more is garbage. GT Sport's race
        // summary quads read float vertex data as their V#s, different
        // every frame; specializing on them made a new variant and pipeline
        // nearly every frame, and those draws were skipped while each built
        // (the flickering driver figure). Such an attribute counts as plain
        // floats. -DisablePerf 83 specializes on it.
        if (Gcn::VertexAttribute::IsGarbage(sharp)) {
            spec.num_class = AmdGpu::NumberClass::Float;
            spec.dst_select = AmdGpu::CompMapping{
                .r = AmdGpu::CompSwizzle::Red,
                .g = AmdGpu::CompSwizzle::Green,
                .b = AmdGpu::CompSwizzle::Blue,
                .a = AmdGpu::CompSwizzle::Alpha,
            };
            return;
        }
        spec.num_class = AmdGpu::GetNumberClass(sharp.GetNumberFmt());
        spec.dst_select = sharp.DstSelect();
    }

    static void SetBufferSpec(BufferSpecialization& spec, const Info& info, size_t index,
                              AmdGpu::Buffer sharp) {
        const auto& desc = info.buffers[index];
        const bool usage_known = info.resource_usage_known;
        static const bool junk_filter = [] {
            const char* env = std::getenv("SHADGT_BUFFER_JUNK_FILTER");
            return !(env && env[0] == '0');
        }();
        if (junk_filter && !desc.IsSpecial() && (sharp._padding1 != 0 || sharp.type != 0)) {
            spec.stride = 0;
            spec.is_formatted = desc.is_formatted;
            spec.swizzle_enable = 0;
            spec.add_tid_enable = 0;
            spec.coherent = desc.buffer_type == BufferType::SharedMemory;
            return;
        }
        spec.stride =
            !usage_known || info.buffer_stride_used[index] ? sharp.GetStride() : 0;
        if (usage_known) {
            spec.add_tid_enable = sharp.add_tid_enable;
            spec.coherent =
                desc.buffer_type == BufferType::SharedMemory || sharp.mtype == 3;
        }
        spec.is_formatted = desc.is_formatted;
        spec.swizzle_enable = sharp.swizzle_enable;
        if (spec.is_formatted) {
            spec.data_format = static_cast<u32>(sharp.GetDataFmt());
            spec.num_format = static_cast<u32>(sharp.GetNumberFmt());
            spec.dst_select = sharp.DstSelect();
            spec.num_conversion = sharp.GetNumberConversion();
        }
        if (spec.swizzle_enable) {
            spec.index_stride = sharp.index_stride;
            spec.element_size = sharp.element_size;
        }
    }

    static bool BoundEmptyForMatch(const BufferResource& desc, AmdGpu::Buffer sharp) {
        static const bool enabled = Common::PerfFeatureEnabled(172);
        return enabled && !desc.IsSpecial() && IsBoundEmpty(sharp);
    }

    bool MatchesBufferVertex(const Info& current, const RuntimeInfo& runtime,
                             Backend::Bindings binding) const {
        Gcn::FetchShaderData fetch;
        const bool parsed = Gcn::ParseFetchShader(current, fetch);
        bool equal = Valid() && buffers.size() == current.buffers.size() &&
                     images.empty() && fmasks.empty() && samplers.empty() &&
                     vs_attribs.size() == (parsed ? fetch.attributes.size() : 0) &&
                     runtime_info == runtime && fetch_shader_data == fetch &&
                     (buffers.empty() || start == binding);
        if (parsed) {
            for (size_t i = 0; i < fetch.attributes.size(); ++i) {
                const auto& desc = fetch.attributes[i];
                const auto sharp = desc.GetSharp(current);
                if (equal) {
                    VsAttribSpecialization spec{};
                    if (sharp) SetVertexAttributeSpec(spec, desc, sharp, runtime);
                    equal = vs_attribs[i] == spec;
                }
            }
        }
        for (size_t i = 0; equal && i < current.buffers.size(); ++i) {
            const auto& desc = current.buffers[i];
            const auto sharp = desc.GetSharp(current);
            if (sharp) {
                BufferSpecialization spec{};
                SetBufferSpec(spec, current, i, sharp);
                equal = BoundEmptyForMatch(desc, sharp) ? buffers[i].SameButAddressing(spec)
                                                       : buffers[i] == spec;
            }
        }
        return equal;
    }

    /// PERF-GT7-058: whether PackInputs may stand for a new specialization of this stage. Not
    /// tessellation stages (their runtime info comes from memory).
    /// FIX-GT7-082: nor a vertex stage with a fetch shader. Its attributes' V#s are read from guest
    /// memory at each read, not from the flattened buffer, so the pack recorded after a full
    /// match could hold another V# than the one that match was made from (a float attribute that
    /// became integer in between), and later draws would take the float module for it with no
    /// full comparison. The packed path saved nothing on vertex stages anyway (GT7 race, 11-04:
    /// 0.58-0.64 us a call without it, 0.63-0.68 with it).
    static bool Packable(const Info& info) {
        return info.sw_stage != SwStage::TessellationControl &&
               info.sw_stage != SwStage::TessellationEval &&
               !(info.sw_stage == SwStage::Vertex && info.has_fetch_shader);
    }

private:
    struct FragmentWords {
        std::array<u64, 4 + 2 * NUM_BUFFERS + 2 * NUM_IMAGES + NUM_FMASKS + NUM_SAMPLERS> words;
        u64* next = words.data();

        void push_back(u64 word) {
            DEBUG_ASSERT(next < words.data() + words.size());
            *next++ = word;
        }
    };

    struct ImageFormat {
        u8 number;
        u8 conversion;
    };

    static ImageFormat FragmentImageFormat(const AmdGpu::Image& sharp) {
        static constexpr auto table = [] {
            std::array<ImageFormat, 1024> result{};
            for (u32 i = 0; i < result.size(); ++i) {
                const auto data = static_cast<AmdGpu::DataFormat>(i & 63);
                const auto number = static_cast<AmdGpu::NumberFormat>(i >> 6);
                result[i] = {u8(AmdGpu::RemapNumberFormat(number, data)),
                             u8(AmdGpu::MapNumberConversion(number, data))};
            }
            return result;
        }();
        return table[sharp.data_format | (sharp.num_format << 6)];
    }

public:
    /// PERF-GT7-058: what the constructor above reads of the sharps and of the fetch shader, as the
    /// values it derives from them, packed into words without making a specialization. A stage
    /// with equal words, an equal runtime info, start bindings and fetch shader data, and the same
    /// info makes an equal specialization, for a stage Packable allows. Keep in step with the
    /// constructor.
    static void PackInputs(const Info& info, std::vector<u64>& out, Gcn::FetchShaderData& fetch) {
        WritePackedInputs(info, out, &fetch);
    }

    static bool MatchesFragmentInputs(const Info& info, std::span<const u64> expected,
                                      const Gcn::FetchShaderData& saved_fetch) {
        DEBUG_ASSERT(info.sw_stage == SwStage::Fragment);
        FragmentWords out;
        WritePackedInputs(info, out, nullptr);
        return std::equal(out.words.data(), out.next, expected.begin(), expected.end()) &&
               Gcn::FetchShaderData{} == saved_fetch;
    }

private:
    template <typename Words>
    static void WritePackedInputs(const Info& info, Words& out, Gcn::FetchShaderData* fetch) {
        const auto mapping = [](const AmdGpu::CompMapping& m) {
            return u64(m.array[0]) | u64(m.array[1]) << 8 | u64(m.array[2]) << 16 |
                   u64(m.array[3]) << 24;
        };
        constexpr bool fragment_key = std::is_same_v<Words, FragmentWords>;
        if constexpr (!fragment_key) {
            *fetch = {};
            if (info.sw_stage == SwStage::Vertex && Gcn::ParseFetchShader(info, *fetch)) {
                for (const auto& desc : fetch->attributes) {
                    const auto sharp = desc.GetSharp(info);
                    if (!sharp) {
                        out.push_back(0);
                    } else if (Gcn::VertexAttribute::IsGarbage(sharp)) {
                        out.push_back(1);
                    } else {
                        out.push_back(2 | u64(AmdGpu::GetNumberClass(sharp.GetNumberFmt())) << 8 |
                                      mapping(sharp.DstSelect()) << 16);
                    }
                }
            }
        }
        out.push_back(0xb0f5);
        const bool usage_known = info.resource_usage_known;
        static const bool empty_without_addressing = Common::PerfFeatureEnabled(172);
        for (size_t i = 0; i < info.buffers.size(); ++i) {
            const auto& desc = info.buffers[i];
            const auto sharp = desc.GetSharp(info);
            if (!sharp) {
                out.push_back(0);
                continue;
            }
            u64 w = 1;
            // Keep empty-address buffers distinct even where directional matching
            // permits them to reuse an existing specialization. FIX-GT7-024: as are those the
            // rasterizer binds empty (IsBoundEmpty), as the constructor marks them.
            if (!desc.IsSpecial() &&
                (sharp.base_address == 0 || (empty_without_addressing && IsBoundEmpty(sharp)))) {
                w |= 2;
            }
            w |= u64(!usage_known || info.buffer_stride_used[i] ? sharp.GetStride() : 0) << 2;
            if (usage_known) {
                w |= u64(sharp.add_tid_enable) << 16;
                w |= u64(desc.buffer_type == BufferType::SharedMemory || sharp.mtype == 3) << 17;
            }
            w |= u64(desc.is_formatted) << 18;
            w |= u64(sharp.swizzle_enable) << 19;
            if (desc.is_formatted) {
                w |= u64(sharp.GetDataFmt()) << 20;
                w |= u64(sharp.GetNumberFmt()) << 28;
                w |= mapping(sharp.DstSelect()) << 32;
            }
            u64 w2 = desc.is_formatted ? u64(sharp.GetNumberConversion()) : 0;
            if (sharp.swizzle_enable) {
                w2 |= u64(sharp.index_stride) << 32 | u64(sharp.element_size) << 40;
            }
            out.push_back(w);
            out.push_back(w2);
        }
        out.push_back(0x1a9e);
        for (size_t i = 0; i < info.images.size(); ++i) {
            const auto& desc = info.images[i];
            const auto sharp = desc.GetSharp(info);
            // Unbound images too: the constructor records what translation saw (SHD-003).
            u64 w = sharp ? 1 : 0;
            const auto type = FMaskKeyEnabled() && sharp.Valid() && AmdGpu::IsFmask(sharp.GetDataFmt())
                                  ? AmdGpu::ImageType::Invalid
                                  : sharp.GetViewType(desc.is_array);
            w |= u64(type) << 1;
            const auto number = [&] {
                if constexpr (fragment_key) {
                    return AmdGpu::NumberFormat(FragmentImageFormat(sharp).number);
                } else {
                    return sharp.GetNumberFmt();
                }
            }();
            w |= u64(AmdGpu::IsInteger(number)) << 8;
            w |= u64(desc.is_written) << 9;
            if (desc.is_written) {
                w |= mapping(sharp.DstSelect()) << 16;
            } else if (!usage_known || info.image_srgb_used[i]) {
                w |= u64(number == AmdGpu::NumberFormat::Srgb) << 10;
            }
            w |= u64(desc.NumBindings(info)) << 48;
            out.push_back(w);
            if constexpr (fragment_key) {
                out.push_back(FragmentImageFormat(sharp).conversion);
            } else {
                out.push_back(u64(sharp.GetNumberConversion()));
            }
        }
        out.push_back(0xf3a5);
        for (const auto& desc : info.fmasks) {
            const auto sharp = desc.GetSharp(info);
            if (!sharp) {
                out.push_back(0);
                continue;
            }
            u64 width = sharp.width;
            u64 height = sharp.height;
            if (FMaskKeyEnabled()) {
                width = 0;
                height = 0;
                if (sharp.Valid() && AmdGpu::IsFmask(sharp.GetDataFmt())) {
                    width = desc.size_read ? sharp.width + 1 : 1;
                    height = desc.size_read ? sharp.height + 1 : 1;
                }
            }
            out.push_back(1 | width << 8 | height << 32);
        }
        out.push_back(0x5a3b);
        for (const auto& desc : info.samplers) {
            const auto sharp = desc.GetSharp(info);
            out.push_back(!sharp ? 0
                                 : 1 | u64(sharp.force_unnormalized) << 1 |
                                       u64(sharp.force_degamma) << 2);
        }
    }

public:
    void ForEachSharp(auto& spec_list, auto& desc_list, auto&& func) {
        for (const auto& desc : desc_list) {
            auto& spec = spec_list.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            if (!sharp) {
                continue;
            }
            func(spec, desc, sharp);
        }
    }

    void ForEachSharp(u32& binding, auto& spec_list, auto& desc_list, auto&& func,
                      bool record_unbound = false) {
        for (const auto& desc : desc_list) {
            auto& spec = spec_list.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            if (!sharp) {
                binding++;
                if (record_unbound) {
                    // FIX-043: what translation saw for the unbound resource, so a bound one
                    // later is compared with the code's inputs instead of with zeros.
                    func(spec, desc, sharp);
                }
                continue;
            }
            bitset[binding++] = true;
            func(spec, desc, sharp);
        }
    }

    [[nodiscard]] bool Valid() const {
        return info != nullptr;
    }

    /// FIX-GT7-024: a V# with records that the rasterizer still binds empty, so reads give 0
    /// and writes are dropped: a base of 0, or a buffer ending past the GPU's 40-bit address
    /// space (FIX-031). GT7's city shaders leave V#s made of float constants (strides like
    /// 0x3e2a, terabytes long) in slots a draw doesn't read; their values changed from one run
    /// to the next, and each one made a new translation and a pipeline compile of up to a second
    /// (the building drawn black meanwhile). Such a buffer is compared without its addressing.
    static bool IsBoundEmpty(const AmdGpu::Buffer& sharp) {
        constexpr u64 GpuAddressLimit = 1ULL << 40;
        const u64 base = sharp.base_address;
        const u64 size = sharp.GetSize();
        return sharp.num_records != 0 &&
               (base == 0 || (sharp.num_records != UINT32_MAX &&
                              (size >= GpuAddressLimit || base + size > GpuAddressLimit)));
    }

    /// The right side's buffer `i` at `binding` matches this one's (FIX-GT7-024: without the
    /// addressing when the right side binds it empty).
    bool BufferMatches(const StageSpecialization& other, u32 i, u32 binding) const {
        return other.bound_empty[binding] ? buffers[i].SameButAddressing(other.buffers[i])
                                          : buffers[i] == other.buffers[i];
    }

    bool operator==(const StageSpecialization& other) const {
        if (!Valid() || !other.Valid()) {
            return false;
        }

        // Resource discovery can differ between permutations of the same program. Check
        // lengths before comparing individual resources or treating an empty layout as reusable.
        if (buffers.size() != other.buffers.size() || images.size() != other.images.size() ||
            samplers.size() != other.samplers.size()) {
            return false;
        }

        if (vs_attribs != other.vs_attribs) {
            return false;
        }

        if (runtime_info != other.runtime_info) {
            return false;
        }

        if (fetch_shader_data != other.fetch_shader_data) {
            return false;
        }

        if (fmasks != other.fmasks) {
            return false;
        }

        // For VS which only generates geometry and doesn't have any inputs, its start
        // bindings still may change as they depend on previously processed FS. The check below
        // handles this case and prevents generation of redundant permutations. This is also safe
        // for other types of shaders with no bindings.
        if (buffers.empty() && images.empty() && samplers.empty()) {
            return true;
        }

        if (start != other.start) {
            return false;
        }

        u32 binding{};
        for (u32 i = 0; i < buffers.size(); i++, binding++) {
            if (other.bitset[binding] && !BufferMatches(other, i, binding)) {
                return false;
            }
        }
        for (u32 i = 0; i < images.size(); i++) {
            if (other.bitset[binding++] && images[i] != other.images[i]) {
                return false;
            }
        }

        for (u32 i = 0; i < samplers.size(); i++) {
            if (samplers[i] != other.samplers[i]) {
                return false;
            }
        }
        return true;
    }

    /// Diagnostic: names the first component that makes operator== fail, following its order.
    /// Returns nullptr when the specializations compare equal.
    const char* FirstDifference(const StageSpecialization& other, u32& index) const {
        index = 0;
        if (!Valid() || !other.Valid()) {
            return "invalid";
        }
        if (buffers.size() != other.buffers.size()) {
            return "buffer count";
        }
        if (images.size() != other.images.size()) {
            return "image count";
        }
        if (samplers.size() != other.samplers.size()) {
            return "sampler count";
        }
        if (vs_attribs != other.vs_attribs) {
            return "vs attributes";
        }
        if (runtime_info != other.runtime_info) {
            const auto& a = runtime_info;
            const auto& b = other.runtime_info;
            if (a.hw_stage != b.hw_stage || a.sw_stage != b.sw_stage) {
                return "runtime stage";
            }
            if (!(a.props == b.props)) {
                return "runtime props";
            }
            if (!a.HasSameSwInfo(b.sw)) {
                return "runtime sw info";
            }
            if (a.hw_stage == HwStage::Fragment) {
                const auto& fa = a.hw.fs;
                const auto& fb = b.hw.fs;
                for (u32 i = 0; i < MaxColorBuffers; ++i) {
                    if (!(fa.color_buffers[i] == fb.color_buffers[i])) {
                        index = i;
                        return "fs color buffer";
                    }
                }
                if (!(fa.en_flags == fb.en_flags) || !(fa.addr_flags == fb.addr_flags)) {
                    return "fs input enables";
                }
                if (fa.num_inputs != fb.num_inputs) {
                    return "fs input count";
                }
                if (fa.z_export_format != fb.z_export_format || fa.mrtz_mask != fb.mrtz_mask) {
                    return "fs depth export";
                }
                if (fa.num_samples != fb.num_samples) {
                    return "fs samples";
                }
                if (fa.front_face_all_bits != fb.front_face_all_bits) {
                    return "fs front face";
                }
                if (fa.dual_source_blending != fb.dual_source_blending) {
                    return "fs dual source";
                }
                if (fa.clip_distance_emulation != fb.clip_distance_emulation) {
                    return "fs clip distance";
                }
                if (fa.vertex_order_check != fb.vertex_order_check) {
                    return "fs vertex order check";
                }
                for (u32 i = 0; i < fa.num_inputs; ++i) {
                    if (!(fa.inputs[i] == fb.inputs[i])) {
                        index = i;
                        return "fs input";
                    }
                }
            }
            return "runtime hw info";
        }
        if (fetch_shader_data != other.fetch_shader_data) {
            return "fetch shader";
        }
        if (fmasks != other.fmasks) {
            return "fmasks";
        }
        if (buffers.empty() && images.empty() && samplers.empty()) {
            return nullptr;
        }
        if (start != other.start) {
            return "start bindings";
        }
        u32 binding{};
        for (u32 i = 0; i < buffers.size(); i++, binding++) {
            if (other.bitset[binding] && !BufferMatches(other, i, binding)) {
                index = i;
                return other.bound_empty[binding] ? "buffer bound empty" : "buffer";
            }
        }
        for (u32 i = 0; i < images.size(); i++) {
            if (other.bitset[binding++] && images[i] != other.images[i]) {
                index = i;
                return "image";
            }
        }
        for (u32 i = 0; i < samplers.size(); i++) {
            if (samplers[i] != other.samplers[i]) {
                index = i;
                return "sampler";
            }
        }
        return nullptr;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
