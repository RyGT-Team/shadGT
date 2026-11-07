// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/shader_usage_serde.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#ifdef RYTY_GPU
#include "video_core/renderer_vulkan/vk_rasterizer_native.h"
#endif

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
// Synthetic dual-source exports change the fragment shader interface. Version 13 adds the
// general swizzled factor blend, which also changes the runtime color-buffer layout.
static constexpr u32 ShaderBinaryVersion = 271u + Shader::Backend::PushDataLayoutRevision;
// Only masked fragment permutations need the extra push constants. Keep ordinary cached
// shaders compatible; older renderers must skip the new permutations on rollback.
static constexpr u32 ShaderBinaryVersionQuadSampleMask = 272u + Shader::Backend::PushDataLayoutRevision;
static constexpr u32 ShaderBinaryVersionMsaaRead = 273u + Shader::Backend::PushDataLayoutRevision;
static constexpr u32 ShaderBinaryVersionMsaaReadQuadSampleMask = 274u + Shader::Backend::PushDataLayoutRevision;
// Scalar-pointer compute loads support bounded prefixes and prologue table loads.
static constexpr u32 ShaderBinaryVersionScalarPrefix = 275u + Shader::Backend::PushDataLayoutRevision;
static constexpr u32 ShaderBinaryVersionScalarPrefixMsaaRead = 276u + Shader::Backend::PushDataLayoutRevision;
// Vertex fetch conversion/swizzles must agree with the invalid-descriptor cache policy.
static constexpr u32 ShaderBinaryVersionVertexFetch = 277u + Shader::Backend::PushDataLayoutRevision;
// Shader semantic revision 0x100 invalidates binaries emitted before the MED3 quiet-NaN fix.
// Fragment reflections use cube views; other stages keep their existing array path.
static constexpr u32 ShaderBinaryVersionNativeCube = 279u + Shader::Backend::PushDataLayoutRevision;
// AMD single-sample KHR barycentrics change the fragment interface and interpolation.
static constexpr u32 ShaderBinaryVersionSingleSampleBarycentric = 282u + Shader::Backend::PushDataLayoutRevision;
// Tessellation modules and keys must use the same translation-time layout.
static constexpr u32 ShaderBinaryVersionTessellationLayout = 283u + Shader::Backend::PushDataLayoutRevision;
static constexpr u32 ShaderBinaryVersionTessellationMip = 286u + Shader::Backend::PushDataLayoutRevision;
static constexpr u32 ShaderBinaryVersionTessellationNoMip = 287u + Shader::Backend::PushDataLayoutRevision;
// FIX-335: LDS readers branch before input loads and use bounded, padded scratch.
// Only emulated TCS modules change; unrelated stored shaders remain compatible.
static constexpr u32 ShaderBinaryVersionTessellationLdsBoundsMip = 288u + Shader::Backend::PushDataLayoutRevision;
static constexpr u32 ShaderBinaryVersionTessellationLdsBoundsNoMip = 289u + Shader::Backend::PushDataLayoutRevision;
// FIX-223: fragment shaders whose discards end the pixel (AMD). 285: FIX-GTS-011 keeps some
// shaders' discarded pixels as helpers again.
static constexpr u32 ShaderBinaryVersionDiscardKill = 285u + Shader::Backend::PushDataLayoutRevision;

static bool UsesScalarPrefix(const Shader::Info& info) {
    return info.hw_stage == Shader::HwStage::Compute &&
           std::ranges::any_of(info.buffers, [](const auto& buffer) {
               return buffer.post_op == Shader::SharpFetchPostOp::ReadConstPointer;
           });
}
static_assert(sizeof(Shader::HwFragmentRuntimeInfo) == 248 && sizeof(Shader::RuntimeInfo) == 312,
              "Legacy metadata compatibility requires the runtime-info layout to stay unchanged");
// Stored output metadata and runtime color-buffer flags include swizzled-alpha emulation.
// Version 13: stored SRT walker code also records flattened source addresses.
// Version 14: stored specializations no longer include compile-time runtime-info changes.
// 15: shader attribute loads/stores are stored (FIX-012). Version 14 entries still load, with
// their flags recovered on first use by a rect/quad-list pipeline.
// 16: the resource usage FIX-043 keys permutations by follows the Info. Versions 14 and 15
// still load, as translations of unknown usage (keyed on every resource property, as stored).
// 17: worker translations own resolved descriptor inputs and compile-time keys.
// FMask descriptors now store their complete fetch and size-use flag.
static constexpr u32 ShaderMetaVersion = 18u;
static constexpr u32 ShaderMetaVersionWithoutResourceUsage = 15u;
static constexpr u32 ShaderMetaVersionWithoutAttributeFlags = 14u;
// 9: rect/quad-list helper shaders built from complete attribute info (FIX-012). Version 8
// entries still load unless they carry such helper shaders, which may have been built from a
// shader without flags.
// 10: quad-list helper control shaders without the local index array (FIX-013). Entries of
// versions 8 and 9 still load unless they carry rect/quad-list helper shaders.
static constexpr u32 PipelineKeyVersion = 10u;
static constexpr u32 PipelineKeyVersionWithoutAttributeFlags = 8u;
static constexpr u32 PipelineKeyVersionOldQuadHelpers = 9u;
} // namespace Serialization

namespace Vulkan {

static std::atomic<bool> single_sample_barycentric_cache_enabled{false};
// AMD-307: hw.fs.vertex_order_check was padding before. Where the check is off (every device
// except AMD's older compiler) it is cleared on load, so old stores keep matching; where it is on,
// the shader store has its own version (cache_storage.cpp) and holds no older entries.
static std::atomic<bool> vertex_order_check_cache_enabled{false};
static std::atomic<bool> native_cube_cache_enabled{false};
void SetNativeCubeCacheEnabled(bool enabled) {
    native_cube_cache_enabled.store(enabled, std::memory_order_relaxed);
}
void SetVertexOrderCheckCacheEnabled(bool enabled) {
    vertex_order_check_cache_enabled.store(enabled, std::memory_order_relaxed);
}
void SetSingleSampleBarycentricCacheEnabled(bool enabled) {
    single_sample_barycentric_cache_enabled.store(enabled, std::memory_order_relaxed);
}
// FIX-223: fragment shaders translated with discards that end the pixel are stored under their
// own version, so binaries made before (pixels demoted to helpers) are translated again.
static std::atomic<bool> discard_kill_cache_enabled{false};
void SetDiscardKillCacheEnabled(bool enabled) {
    discard_kill_cache_enabled.store(enabled, std::memory_order_relaxed);
}

static bool HasMsaaImage(const Shader::StageSpecialization& spec) {
    return std::ranges::any_of(spec.images, [](const auto& image) {
        return image.type == AmdGpu::ImageType::Color2DMsaa ||
               image.type == AmdGpu::ImageType::Color2DMsaaArray;
    });
}

static u32 ShaderBinaryVersionFor(const Shader::StageSpecialization& spec,
                                  bool scalar_prefix = false) {
    if (spec.runtime_info.sw_stage == Shader::SwStage::TessellationControl) {
        static const bool mip = [] {
            const char* env = std::getenv("RYGT_UNNORMALIZED_MIP");
            return !(env && env[0] == '0');
        }();
        if (spec.runtime_info.sw.tcs.lds_emulation) {
            return mip ? Serialization::ShaderBinaryVersionTessellationLdsBoundsMip
                       : Serialization::ShaderBinaryVersionTessellationLdsBoundsNoMip;
        }
        return mip ? Serialization::ShaderBinaryVersionTessellationMip
                   : Serialization::ShaderBinaryVersionTessellationNoMip;
    }
    if (spec.runtime_info.sw_stage == Shader::SwStage::TessellationControl ||
        spec.runtime_info.sw_stage == Shader::SwStage::TessellationEval) {
        return Serialization::ShaderBinaryVersionTessellationLayout;
    }
    if (spec.runtime_info.hw_stage == Shader::HwStage::Fragment) {
        bool native_cube = false;
#ifdef RYTY_GPU
        native_cube = native_cube_cache_enabled.load(std::memory_order_relaxed) && std::ranges::any_of(spec.images, [](const auto& image) {
            return image.type == AmdGpu::ImageType::Cube;
        });
#endif
        const u32 fragment = FragmentBinaryVersion(
            discard_kill_cache_enabled.load(std::memory_order_relaxed),
            spec.runtime_info.hw.fs.single_sample_barycentric, native_cube,
            Serialization::ShaderBinaryVersionDiscardKill,
            Serialization::ShaderBinaryVersionSingleSampleBarycentric,
            Serialization::ShaderBinaryVersionNativeCube);
        if (fragment) return fragment;
    }
    if (!spec.fetch_shader_data.Empty()) {
        return Serialization::ShaderBinaryVersionVertexFetch;
    }
    if (scalar_prefix) {
        return HasMsaaImage(spec) ? Serialization::ShaderBinaryVersionScalarPrefixMsaaRead
                                  : Serialization::ShaderBinaryVersionScalarPrefix;
    }
    const bool masked_fragment = spec.runtime_info.hw_stage == Shader::HwStage::Fragment &&
                                 spec.runtime_info.hw.fs.quad_sample_mask;
    if (HasMsaaImage(spec)) {
        return masked_fragment ? Serialization::ShaderBinaryVersionMsaaReadQuadSampleMask
                               : Serialization::ShaderBinaryVersionMsaaRead;
    }
    return masked_fragment ? Serialization::ShaderBinaryVersionQuadSampleMask
                           : Serialization::ShaderBinaryVersion;
}

static bool IsKnownShaderBinaryVersion(u32 version) {
    // Recognize old metadata for prewarm discovery, but LoadShaderMeta below rejects its
    // binary revision and retranslates it. Never relabel an old binary as corrected.
    version = UntagCubeVersion(version);
    version |= 0x100u;
    // Old metadata remains discoverable for prewarm and index reservation. The
    // exact comparison in LoadShaderMeta still rejects its incompatible SPIR-V.
    version |= Shader::Backend::PushDataLayoutRevision;
    if (version == Serialization::ShaderBinaryVersionTessellationLdsBoundsMip ||
        version == Serialization::ShaderBinaryVersionTessellationLdsBoundsNoMip ||
        version == Serialization::ShaderBinaryVersionTessellationMip ||
        version == Serialization::ShaderBinaryVersionTessellationNoMip) return true;
    if (version == Serialization::ShaderBinaryVersionTessellationLayout) return true;
    if (version == Serialization::ShaderBinaryVersionDiscardKill) {
        return discard_kill_cache_enabled.load(std::memory_order_relaxed);
    }
    if (version == Serialization::ShaderBinaryVersionSingleSampleBarycentric) {
        return single_sample_barycentric_cache_enabled.load(std::memory_order_relaxed);
    }
    return version == Serialization::ShaderBinaryVersion ||
           version == Serialization::ShaderBinaryVersionQuadSampleMask ||
           version == Serialization::ShaderBinaryVersionMsaaRead ||
           version == Serialization::ShaderBinaryVersionMsaaReadQuadSampleMask ||
           version == Serialization::ShaderBinaryVersionScalarPrefix ||
           version == Serialization::ShaderBinaryVersionScalarPrefixMsaaRead ||
           version == Serialization::ShaderBinaryVersionVertexFetch ||
           version == Serialization::ShaderBinaryVersionNativeCube;
}

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

bool PeekShaderMeta(std::vector<u8>&& blob, u64& pgm_hash, size_t& perm_idx) {
    // The layout LoadShaderMeta reads: versions, permutation hash and index, the
    // specialization, then the shader info, which starts with InfoPersistent.
    Serialization::Archive ar{std::move(blob)};
    Serialization::Reader meta{ar};
    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }
    u32 binary_version{};
    meta.Read(binary_version);
    if (!IsKnownShaderBinaryVersion(binary_version)) {
        return false;
    }
    u64 perm_hash{};
    meta.Read(perm_hash);
    meta.Read(perm_idx);
    Shader::StageSpecialization spec{};
    spec.Deserialize(ar);
    alignas(Shader::InfoPersistent) std::array<u8, sizeof(Shader::InfoPersistent)> persistent;
    meta.Read(persistent.data(), persistent.size());
    pgm_hash = reinterpret_cast<const Shader::InfoPersistent*>(persistent.data())->pgm_hash;
    return true;
}

bool LoadStoredGraphicsPipeline(std::vector<u8>&& blob, GraphicsPipelineKey& key,
                                GraphicsPipeline::SerializationSupport& sdata) {
    if (blob.size() < 2 * sizeof(u32) + sizeof(GraphicsPipelineKey)) {
        return false;
    }
    Serialization::Archive ar{std::move(blob)};
    Serialization::Reader pldata{ar};
    u32 version{};
    pldata.Read(version);
    if (version != Serialization::PipelineKeyVersion) {
        return false;
    }
    u32 is_compute{};
    pldata.Read(is_compute);
    if (is_compute != 0) {
        return false;
    }
    key.Deserialize(ar);
    return sdata.Deserialize(ar);
}

namespace {
// SHD-006 census: how many new translations one made before any draw, from a program's code and
// registers alone, would have matched: null sharps (2D/array float textures, unstrided raw
// buffers, plain samplers), pixel inputs in order, plain float targets with the packed export.
// Logged every 250 new translations with what the others needed.
struct TranslationCensus {
    std::mutex mutex;
    u64 total{}, ps{}, ps_default{}, vs{}, vs_default{}, other{};
    std::map<std::string, u64> reasons;
} g_census;

void CountTranslation(const Shader::Info& info, const Shader::StageSpecialization& spec) {
    std::string why;
    const auto add = [&](const char* reason) {
        if (why.find(reason) == std::string::npos) {
            why += why.empty() ? "" : "+";
            why += reason;
        }
    };
    using Type = AmdGpu::ImageType;
    for (const auto& image : spec.images) {
        if (image.type == Type::Cube) {
            add("cube");
        } else if (image.type == Type::Color3D) {
            add("3d");
        } else if (image.type == Type::Color2DMsaa || image.type == Type::Color2DMsaaArray) {
            add("msaa");
        }
        if (image.is_integer) {
            add("int-texture");
        }
        if (image.is_srgb) {
            add("srgb");
        }
        if (image.num_conversion != AmdGpu::NumberConversion::None) {
            add("tex-conversion");
        }
    }
    for (const auto& buffer : spec.buffers) {
        if (buffer.stride != 0) {
            add("stride");
        }
        if (buffer.is_formatted) {
            add("formatted-buffer");
        }
    }
    for (const auto& sampler : spec.samplers) {
        if (sampler.force_unnormalized || sampler.force_degamma) {
            add("sampler");
        }
    }
    const bool is_ps = info.sw_stage == Shader::SwStage::Fragment;
    const bool is_vs = info.sw_stage == Shader::SwStage::Vertex;
    if (is_ps) {
        const auto& fs = spec.runtime_info.hw.fs;
        for (u32 i = 0; i < fs.num_inputs && i < fs.inputs.size(); ++i) {
            if (fs.inputs[i].param_index != i || fs.inputs[i].is_default) {
                add("inputs");
                break;
            }
        }
        for (const auto& cb : fs.color_buffers) {
            if (cb.data_format == AmdGpu::DataFormat::FormatInvalid) {
                continue;
            }
            const bool plain = cb.num_format == AmdGpu::NumberFormat::Float &&
                               cb.num_conversion == AmdGpu::NumberConversion::None &&
                               cb.export_format == AmdGpu::ShaderExportFormat::ABGR_FP16 &&
                               cb.swizzle == AmdGpu::IdentityMapping && !cb.blend_self_scale &&
                               !cb.blend_swizzled_alpha && !cb.blend_swizzled_factors;
            if (!plain) {
                add("outputs");
                break;
            }
        }
    }
    if (is_vs && !spec.vs_attribs.empty()) {
        add("vertex-layout");
    }
    std::scoped_lock lock{g_census.mutex};
    ++g_census.total;
    if (is_ps) {
        ++g_census.ps;
        g_census.ps_default += why.empty() ? 1 : 0;
    } else if (is_vs) {
        ++g_census.vs;
        g_census.vs_default += why.empty() ? 1 : 0;
    } else {
        ++g_census.other;
    }
    if (!why.empty()) {
        ++g_census.reasons[(is_ps ? "ps:" : is_vs ? "vs:" : "other:") + why];
    }
    if (g_census.total % 250 == 0) {
        std::vector<std::pair<u64, std::string>> top;
        for (const auto& [reason, count] : g_census.reasons) {
            top.emplace_back(count, reason);
        }
        std::ranges::sort(top, std::greater{});
        std::string text;
        for (size_t i = 0; i < std::min<size_t>(top.size(), 14); ++i) {
            text += fmt::format(" {}={}", top[i].second, top[i].first);
        }
        LOG_WARNING(Render_Vulkan,
                    "SHD-006 census: {} new translations; pixel {} ({} from code alone), vertex {} "
                    "({} from code alone), other {}; needed:{}",
                    g_census.total, g_census.ps, g_census.ps_default, g_census.vs,
                    g_census.vs_default, g_census.other, text);
    }
}
} // namespace

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    CountTranslation(info, spec); // SHD-006
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(CubeVersionFor(ShaderBinaryVersionFor(spec, Serialization::UsesScalarPrefix(info)),
                              spec, native_cube_cache_enabled.load(std::memory_order_relaxed)));

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);
    WriteResourceUsage(meta, info);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    const bool face_cubes = (binary_version & ClassicCubeTag) != 0;
    binary_version = UntagCubeVersion(binary_version);
    if (!IsKnownShaderBinaryVersion(binary_version)) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    if (spec.runtime_info.hw_stage == Shader::HwStage::Fragment &&
        !vertex_order_check_cache_enabled.load(std::memory_order_relaxed)) {
        spec.runtime_info.hw.fs.vertex_order_check = false; // AMD-307
    }
    if (spec.runtime_info.hw_stage == Shader::HwStage::Fragment &&
        binary_version != Serialization::ShaderBinaryVersionSingleSampleBarycentric) {
        // This byte was padding in every older metadata revision.
        spec.runtime_info.hw.fs.single_sample_barycentric = false;
    }
    if ((binary_version | 0x100u) == Serialization::ShaderBinaryVersion &&
        spec.runtime_info.hw_stage == Shader::HwStage::Fragment) {
        // This byte was padding before quad-mask emulation, and need not have been zero.
        spec.runtime_info.hw.fs.quad_sample_mask = false;
    }
    if (!info.Deserialize(ar, perm_hash_ar,
                          meta_version != Serialization::ShaderMetaVersionWithoutAttributeFlags))
        return false;
    if (!StoredCubesFit(spec, info, face_cubes,
                        native_cube_cache_enabled.load(std::memory_order_relaxed))) return false;
    if (binary_version != ShaderBinaryVersionFor(spec, Serialization::UsesScalarPrefix(info))) {
        return false;
    }
    info.resource_usage_known = false;
    if (meta_version >= Serialization::ShaderMetaVersion) {
        ReadResourceUsage(meta, info);
    }
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

void PipelineCache::Sync() {
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    // FIX-012: rect/quad-list helper shaders forward the vertex outputs the fragment shader
    // reads; a preloaded shader without these flags made them forward nothing.
    info.Write(loads.flags.data(), loads.flags.size());
    info.Write(stores.flags.data(), stores.flags.size());
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar, u64 walker_key, bool has_attribute_flags) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);
    attribute_flags_known = has_attribute_flags;
    if (has_attribute_flags) {
        info.Read(loads.flags.data(), loads.flags.size());
        info.Read(stores.flags.data(), stores.flags.size());
    }

    return srt_info.Deserialize(ar, srt_walker_owner, walker_key);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar, std::shared_ptr<void>& owner, u64 walker_key) {
    Serialization::Reader srt{ar};

    PersistentSrtInfo loaded{};
    if (ar.RemainingBytes() < sizeof(loaded)) return false;
    srt.Read(&loaded, sizeof(loaded));
    loaded.walker_func = nullptr; // Serialized process addresses are never valid here.
    if (loaded.flattened_bufsize_dw < NUM_USER_DATA_REGS ||
        loaded.flattened_bufsize_dw > UINT16_MAX) return false;
    std::shared_ptr<void> next_owner;
    if (loaded.walker_func_size) {
        if (loaded.walker_func_size > ar.RemainingBytes()) return false;
        auto code = RegisterWalkerCode(ar.CurrPtr(), loaded.walker_func_size, walker_key);
        ar.Advance(loaded.walker_func_size);
        loaded.walker_func = code.function;
        next_owner = std::move(code.owner);
    }
    *this = loaded;
    owner = std::move(next_owner);

    return true;
}

#if defined(RYTY_GPU) && defined(_WIN32) && !defined(NDEBUG)
// Exercises the actual cache reader and Info ownership against the built Debug DLL.
extern "C" __declspec(dllexport) int RytySrtWalkerOwnershipSelfTest(char* report, int capacity) {
    if (!report || capacity <= 0) return -1;
    try {
        static_assert(sizeof(PersistentSrtInfo) == 24);
        static_assert(offsetof(PersistentSrtInfo, walker_func_size) == 8);
        static_assert(offsetof(PersistentSrtInfo, flattened_bufsize_dw) == 16);
        // SysV walker: copy UD[0] to flat[16] and its guest address to source[16].
        constexpr u8 bytes[]{0x8B,0x07, 0x89,0x46,0x40,
                             0x48,0x89,0xBA,0x80,0,0,0, 0xC3};
        std::array<u32, NUM_USER_DATA_REGS> data{};
        data[0] = 0x12345678;
        Info original{};
        original.user_data = data;
        auto published = RegisterWalkerCode(bytes, sizeof(bytes), 0x339);
        original.srt_info = {published.function, sizeof(bytes), NUM_USER_DATA_REGS + 1};
        original.srt_walker_owner = std::move(published.owner);
        Info copy = original;
        original.srt_walker_owner.reset();
        copy.RefreshFlatBuf();
        if (copy.flattened_ud_buf[16] != data[0] ||
            copy.flattened_ud_src[16] != reinterpret_cast<u64>(data.data())) return -2;
        Serialization::Archive archive;
        copy.srt_info.Serialize(archive);
        auto wire = archive.TakeOff();
        if (wire.size() != sizeof(PersistentSrtInfo) + sizeof(bytes) ||
            std::memcmp(wire.data(), &copy.srt_info, sizeof(PersistentSrtInfo)) != 0) return -3;
        Info loaded{};
        loaded.user_data = data;
        Serialization::Archive input{std::vector<u8>{wire}};
        if (!loaded.srt_info.Deserialize(input, loaded.srt_walker_owner, 0x339) ||
            !loaded.srt_walker_owner || loaded.srt_info.walker_func == copy.srt_info.walker_func) return -4;
        copy.srt_walker_owner.reset();
        loaded.RefreshFlatBuf();
        if (loaded.flattened_ud_buf[16] != data[0] ||
            loaded.flattened_ud_src[16] != reinterpret_cast<u64>(data.data())) return -5;
        const auto keep_function = loaded.srt_info.walker_func;
        const auto keep_owner = loaded.srt_walker_owner;
        for (int which = 0; which < 4; ++which) {
            auto damaged = wire;
            if (which == 0) damaged.resize(sizeof(PersistentSrtInfo) - 1);
            if (which == 1) damaged.resize(damaged.size() - 1);
            if (which == 2) { const size_t huge = SIZE_MAX; std::memcpy(damaged.data() + 8, &huge, sizeof(huge)); }
            if (which == 3) { const u32 huge = UINT32_MAX; std::memcpy(damaged.data() + 16, &huge, sizeof(huge)); }
            Serialization::Archive bad{std::move(damaged)};
            if (loaded.srt_info.Deserialize(bad, loaded.srt_walker_owner, 0x339) ||
                loaded.srt_info.walker_func != keep_function || loaded.srt_walker_owner != keep_owner) return -6;
            loaded.RefreshFlatBuf();
            if (loaded.flattened_ud_buf[16] != data[0]) return -7;
        }
        std::snprintf(report, static_cast<size_t>(capacity),
            "passed: legacy 24-byte wire record, owned Info copy, cached execution/address, distinct cached code, four malformed records preserve prior owner");
        return 0;
    } catch (const std::exception& e) {
        std::snprintf(report, static_cast<size_t>(capacity), "failed: %s", e.what());
        return -8;
    }
}
#endif

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (!fetch_shader_data.Empty()) {
        spec.Write(sizeof(fetch_shader_data));
        fetch_shader_data.Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader

#include <filesystem>
#include <fstream>
extern "C" __declspec(dllexport) int RytyTraceTessMeta(const char* directory, const char* destination) {
    try {
        std::string result="["; unsigned count=0;
        for(const auto& path:std::filesystem::directory_iterator(directory)) {
            if(path.path().extension()!=".meta")continue;
            std::ifstream file(path.path(),std::ios::binary);
            std::vector<u8> bytes((std::istreambuf_iterator<char>(file)),{});
            if(bytes.size()>16*1024*1024)continue;
            const u64 hashes[]={0xf8543343ull,0xeae56230ull};bool found=false;
            for(auto hash:hashes)for(size_t i=0;i+8<=bytes.size();++i){u64 value;std::memcpy(&value,bytes.data()+i,8);if(value==hash){found=true;break;}}
            if(!found)continue;
            Serialization::Archive ar{std::move(bytes)};Shader::Info info{};Shader::StageSpecialization spec{};size_t permutation{};
            if(!Vulkan::LoadShaderMeta(ar,info,spec,permutation))continue;
            if(info.pgm_hash!=hashes[0]&&info.pgm_hash!=hashes[1])continue;
            if(count++)result+=",";
            const auto stride=info.sw_stage==Shader::SwStage::TessellationEval?spec.runtime_info.sw.tes.hs_output_cp_stride:spec.runtime_info.sw.tcs.hs_output_cp_stride;
            result+=fmt::format(R"({{"file":"{}","hash":{},"stage":{},"stride":{},"pointer_register":{},"descriptor_dword":{},"flat_dwords":{},"buffers":[)",path.path().filename().string(),info.pgm_hash,u32(info.sw_stage),stride,u32(info.tess_consts_ptr_base),info.tess_consts_dword_offset,info.flattened_ud_buf.size());
            unsigned n=0;for(const auto& buffer:info.buffers){if(buffer.IsSpecial())continue;const auto sharp=buffer.GetSharp(info);std::array<u32,4> raw{};std::memcpy(raw.data(),&sharp,16);if(n++)result+=",";result+=fmt::format(R"({{"address":{},"stride":{},"records":{},"words":[{},{},{},{}]}})",u64(sharp.base_address),u32(sharp.stride),u32(sharp.num_records),raw[0],raw[1],raw[2],raw[3]);}
            result+="]}";
            auto bin=std::filesystem::path(destination).parent_path()/(path.path().filename().string()+".flat");std::ofstream flat(bin,std::ios::binary);flat.write(reinterpret_cast<const char*>(info.flattened_ud_buf.data()),info.flattened_ud_buf.size()*4);
        }
        result+="]";std::ofstream report(destination);report<<result;return count;
    }catch(...){return -1;}
}
