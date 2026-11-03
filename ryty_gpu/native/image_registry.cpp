#include "ryty_gpu/native/unique_upload_batch.h"
#include "ryty_gpu/native/upload_cause.h"
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cstring>

#include "common/logging/log.h"
#include "core/guest_hash.h"
#include "core/memory.h"
#include "ryty_gpu/native/image_registry.h"
#include "ryty_gpu/native/upload_scratch_capacity.h"
#include "ryty_gpu/native/gpu_checkpoints.h"
#include "ryty_gpu/native/slow_call_trace.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/texture_cache/tiling_specialization.h"
#include <array>
#include <cstdlib>
#include <xxhash.h>

#include "video_core/host_shaders/tiling_macro_128_comp.h"
#include "video_core/host_shaders/tiling_macro_16_comp.h"
#include "video_core/host_shaders/tiling_macro_32_comp.h"
#include "video_core/host_shaders/tiling_macro_64_comp.h"
#include "video_core/host_shaders/tiling_macro_8_comp.h"
#include "video_core/host_shaders/tiling_macro_96_comp.h"
#include "video_core/host_shaders/tiling_micro_128_comp.h"
#include "video_core/host_shaders/tiling_micro_16_comp.h"
#include "video_core/host_shaders/tiling_micro_32_comp.h"
#include "video_core/host_shaders/tiling_micro_64_comp.h"
#include "video_core/host_shaders/tiling_micro_8_comp.h"
#include "video_core/host_shaders/tiling_micro_96_comp.h"

namespace Native {
namespace {

// The untiling shader's uniform (tiling.comp, scalar layout), as the texture cache fills it.
struct TilingParams {
    u32 bank_swizzle;
    u32 micro_tiled_mips;
    u32 num_mips;
    std::array<VideoCore::MipInfo, VideoCore::MAX_MIPS> mips;
};

Rhi::Format ToRhi(vk::Format format) {
    return Rhi::FormatFromVk(static_cast<u32>(format));
}

std::span<const u32> UntileCode(u32 num_bits, bool macro) {
    switch (num_bits) {
    case 8: return macro ? std::span<const u32>{TILING_MACRO_8_COMP} : std::span<const u32>{TILING_MICRO_8_COMP};
    case 16: return macro ? std::span<const u32>{TILING_MACRO_16_COMP} : std::span<const u32>{TILING_MICRO_16_COMP};
    case 32: return macro ? std::span<const u32>{TILING_MACRO_32_COMP} : std::span<const u32>{TILING_MICRO_32_COMP};
    case 64: return macro ? std::span<const u32>{TILING_MACRO_64_COMP} : std::span<const u32>{TILING_MICRO_64_COMP};
    case 96: return macro ? std::span<const u32>{TILING_MACRO_96_COMP} : std::span<const u32>{TILING_MICRO_96_COMP};
    case 128: return macro ? std::span<const u32>{TILING_MACRO_128_COMP} : std::span<const u32>{TILING_MICRO_128_COMP};
    default: return {};
    }
}

Rhi::ImageDesc DescOf(const VideoCore::ImageInfo& info, bool render_target, bool with_targets,
                      bool with_storage) {
    Rhi::ImageDesc desc{};
    desc.format = ToRhi(info.pixel_format);
    desc.width = std::max(info.size.width, 1u);
    desc.height = std::max(info.size.height, 1u);
    desc.mips = std::max<u32>(info.resources.levels, 1);
    desc.layers = std::max<u32>(info.resources.layers, 1);
    desc.samples = std::max(info.num_samples, 1u);
    // As in shadGT's texture cache, 1D images are 2D and cube maps are 2D arrays: the shader
    // translator declares them that way.
    switch (info.type) {
    case AmdGpu::ImageType::Color3D:
        desc.dim = Rhi::ImageDim::D3;
        desc.depth = std::max(info.size.depth, 1u);
        desc.layers = 1;
        break;
    default:
        desc.dim = Rhi::ImageDim::D2;
        break;
    }
    desc.usage = Rhi::ImageUsageSampled | Rhi::ImageUsageTransferSrc | Rhi::ImageUsageTransferDst;
    if (desc.dim == Rhi::ImageDim::D2 && desc.layers >= 6 && desc.width == desc.height) {
        desc.usage |= Rhi::ImageUsageCube;
    }
    if (info.props.is_depth) {
        desc.usage |= Rhi::ImageUsageDepthTarget;
    } else {
        // Views reinterpret colour images in other formats of the same texel size, as the game
        // does with GPU memory.
        desc.usage |= Rhi::ImageUsageMutableFormat;
        if (with_targets && !info.props.is_block) {
            desc.usage |= Rhi::ImageUsageColorTarget;
        }
        if (with_storage && !info.props.is_block && desc.samples == 1) {
            desc.usage |= Rhi::ImageUsageStorage; // compute shaders write pictures too
        }
    }
    (void)render_target;
    return desc;
}

} // namespace

std::atomic<bool> g_dump_request{false};

const std::vector<VAddr>& WatchedTextures() {
#ifndef NDEBUG
    static const std::vector<VAddr> watched = [] {
        std::vector<VAddr> list;
        const char* env = std::getenv("RYGT_NATIVE_WATCH_TEXTURE");
        for (const char* at = env; at && *at;) {
            char* end = nullptr;
            const VAddr address = std::strtoull(at, &end, 16);
            if (end == at) {
                break;
            }
            if (address != 0) {
                list.push_back(address);
            }
            at = *end == ',' ? end + 1 : end;
        }
        return list;
    }();
    return watched;
#else
    static const std::vector<VAddr> empty;
    return empty;
#endif
}

bool WatchedTexture(VAddr begin, VAddr end) {
#ifndef NDEBUG
    for (const VAddr watched : WatchedTextures()) {
        if (begin <= watched && watched < std::max(end, begin + 1)) {
            return true;
        }
    }

    return false;
#else
    (void)begin; (void)end;
    return false;
#endif
}

std::atomic<bool> g_watch_window{false};

bool WatchSample() {
#ifndef NDEBUG
    if (g_watch_window.load(std::memory_order_relaxed)) {
        return true;
    }
    static std::atomic<u32> count{};
    const u32 n = count++;
    return n < 400 || n % 500 == 0;
#else
    return false;
#endif
}

std::string WatchedBytes(VAddr address) {
    std::array<u32, 4> head{};
    if (!Core::RytyMemory::GuardedCopy(head.data(), reinterpret_cast<const void*>(address),
                                       sizeof(head))) {
        return "unreadable";
    }
    return fmt::format("{:08x} {:08x} {:08x} {:08x}", head[0], head[1], head[2], head[3]);
}

bool CompatibleLayerStrides(const VideoCore::ImageInfo& a, const VideoCore::ImageInfo& b) {
    // A Vulkan array view cannot relocate its layers to match a different guest slice
    // stride. Equal pitch and tiling alone do not make differently sized arrays aliases.
    if (a.resources.layers <= 1 && b.resources.layers <= 1) return true;
    if (a.resources.layers == 0 || b.resources.layers == 0 ||
        a.resources.levels != b.resources.levels) return false;
    for (u32 level = 0; level < a.resources.levels; ++level) {
        const auto& x = a.mips_layout[level];
        const auto& y = b.mips_layout[level];
        if (x.size % a.resources.layers != 0 || y.size % b.resources.layers != 0 ||
            x.size / a.resources.layers != y.size / b.resources.layers ||
            x.offset != y.offset || x.pitch != y.pitch || x.height != y.height) return false;
    }
    return true;
}

bool SameLayout(const VideoCore::ImageInfo& a, const VideoCore::ImageInfo& b) {
    if (a.guest_address != b.guest_address || a.num_bits != b.num_bits ||
        a.size != b.size || a.pitch != b.pitch || a.tile_mode != b.tile_mode ||
        a.array_mode != b.array_mode || a.bank_swizzle != b.bank_swizzle ||
        a.alt_tile != b.alt_tile || a.resources != b.resources ||
        a.num_samples != b.num_samples || a.props.is_depth != b.props.is_depth ||
        a.props.is_block != b.props.is_block || a.type != b.type) {
        return false;
    }
    // Equal dimensions do not imply equal byte offsets or padded mip footprints.
    // Keep format reinterpretation, but only over the same guest-memory layout.
    for (u32 level = 0; level < a.resources.levels; ++level) {
        const auto& x = a.mips_layout[level];
        const auto& y = b.mips_layout[level];
        if (x.size != y.size || x.pitch != y.pitch || x.height != y.height || x.offset != y.offset) {
            return false;
        }
    }
    return true;
}

const Rhi::ImageView* ImageEntry::View(Rhi::Device& device, const ImageViewKey& key) {
    std::scoped_lock lock{views_mutex};
    for (const auto& [view_key, view] : views) {
        if (view_key == key) {
            return view.get();
        }
    }
    Rhi::ImageViewDesc desc{};
    desc.dim = key.dim;
    desc.format = key.format;
    desc.swizzle = key.swizzle;
    desc.base_mip = key.base_mip;
    desc.mips = key.mips;
    desc.base_layer = key.base_layer;
    desc.layers = key.layers;
    desc.aspect = key.aspect;
    desc.storage = key.storage;
    auto view = device.CreateImageView(image.get(), desc);
    const Rhi::ImageView* result = view.get();
    if (view) {
        views.emplace_back(key, std::move(view));
    }
    return result;
}

ImageRegistry::ImageRegistry(Rhi::Device& device) : device{device} {
    const std::array<Rhi::BindingSlot, 3> slots{{
        {0, Rhi::BindingType::StorageBuffer, Rhi::StageCompute},
        {1, Rhi::BindingType::StorageBuffer, Rhi::StageCompute},
        {2, Rhi::BindingType::UniformBuffer, Rhi::StageCompute},
    }};
    untile_layout = device.CreatePipelineLayout({.bindings = slots});
}

ImageRegistry::~ImageRegistry() = default;

ImageEntry* ImageRegistry::Find(const VideoCore::ImageInfo& info, bool render_target, u64 frame,
                                bool create, bool exact_format) {
    const bool require_format = exact_format || (render_target && info.num_samples > 1);
#ifndef NDEBUG
    ++stats.lookups;
#endif
    Shard& shard = ShardOf(info.guest_address);
    {
        std::shared_lock lock{shard.mutex};
        const auto [first, last] = shard.images.equal_range(info.guest_address);
        ImageEntry* exact = nullptr;
        for (auto it = first; it != last; ++it) {
            if (require_format && it->second->info.pixel_format != info.pixel_format) {
                continue;
            }
            if (it->second->reused.load(std::memory_order_relaxed)) {
                continue; // FIX-157: stale
            }
            // A texture uploaded before the first draw is not a render target. Create and
            // link a target so its writes become sources for the texture's other views.
            if (render_target && it->second->from_cpu) {
                continue;
            }
            if (SameLayout(it->second->info, info)) {
                exact = it->second.get();
                if (render_target || !exact->from_cpu) {
                    break;
                }
            }
        }
        if (render_target) {
            // FIX-154: targets at the same address in the same layout (pitch, tiling, texel
            // size) are one picture in memory, so every draw goes into the largest of them (not
            // into one of its own size). GT Sport draws the car's interior into a 720x680 corner
            // of the showroom's 1920x1080 post-processing target, and its depth of field reads
            // the whole 1920x1080 picture: drawn into a picture of its own, that corner kept the
            // last frame's blurred result, the brightness the game measured there fell as the
            // interior's exposure rose, and the exposure ran away (black car screens).
            // Only plain pictures of one format: a single-sample colour target of one mip and
            // layer inside a larger one of the same format (scratch-pool reuse with other
            // formats or samples broke the race minimaps).
            ImageEntry* largest = nullptr;
            // RYGT_NATIVE_SHARED_TARGETS=0 turns it off (diagnosis).
            static const bool shared_targets = [] {
                const char* env = std::getenv("RYGT_NATIVE_SHARED_TARGETS");
                return !(env && env[0] == '0');
            }();
            const bool plain = shared_targets && info.num_samples == 1 && !info.props.is_depth &&
                               info.resources.levels == 1 && info.resources.layers == 1;
            for (auto it = first; plain && it != last; ++it) {
                const auto& c = it->second->info;
                if (!it->second->from_cpu && it->second->image &&
                    !it->second->reused.load(std::memory_order_relaxed) && c.pitch == info.pitch &&
                    c.tile_mode == info.tile_mode && c.array_mode == info.array_mode &&
                    c.bank_swizzle == info.bank_swizzle && c.alt_tile == info.alt_tile &&
                    c.mips_layout[0].pitch == info.mips_layout[0].pitch &&
                    (c.size != info.size || c.resources != info.resources || SameLayout(c, info)) &&
                    c.num_bits == info.num_bits &&
                    c.num_samples == 1 && !c.props.is_depth && c.type == info.type &&
                    c.pixel_format == info.pixel_format && c.resources.levels == 1 &&
                    c.resources.layers == 1 && c.size.width >= info.size.width &&
                    c.size.height >= info.size.height &&
                    (!largest || u64(c.size.width) * c.size.height >
                                     u64(largest->info.size.width) * largest->info.size.height)) {
                    largest = it->second.get();
                }
            }
            if (largest) {
#ifndef NDEBUG
                ++stats.hits;
#endif
                if (largest != exact) {
#ifndef NDEBUG
                    ++stats.reused_larger;
#endif
                }
                largest->last_use.store(frame, std::memory_order_relaxed);
                return largest;
            }
        }
        // A target of the texture's own layout is read as it is.
        if (exact && (render_target || !exact->from_cpu)) {
#ifndef NDEBUG
            ++stats.hits;
#endif
            exact->last_use.store(frame, std::memory_order_relaxed);
            return exact;
        }
        if (!render_target && !info.props.is_depth &&
            Vulkan::LiverpoolToVK::IsFormatDepthCompatible(info.pixel_format)) {
            // NAT-040: a depth buffer read as a texture (the T# names R32/R16, the image is
            // D32/D16): the depth image itself, viewed through its depth aspect. NAT-121: before a
            // texture of the same layout (an upload of the memory under the depth buffer, made
            // before it existed, would hide it: GT Sport's shadow cascades, a 256x256 depth
            // target with 14 layers read with 13). NAT-126: only for depth-like formats (a
            // B10G11R11 bloom picture in the scratch pool matched a bigger depth buffer there).
            for (auto it = first; it != last; ++it) {
                const auto& cached = it->second->info;
                if (cached.props.is_depth && cached.num_bits == info.num_bits &&
                    !it->second->reused.load(std::memory_order_relaxed) &&
                    cached.resources.layers >= info.resources.layers &&
                    CompatibleLayerStrides(cached, info) &&
                    ((cached.size.width == info.size.width &&
                      cached.size.height == info.size.height) ||
                     (cached.pitch == info.pitch && cached.size.width >= info.size.width &&
                      cached.size.height >= info.size.height))) { // NAT-113
#ifndef NDEBUG
                    ++stats.hits;
#endif
                    it->second->last_use.store(frame, std::memory_order_relaxed);
                    return it->second.get();
                }
            }
        }
        if (exact) {
#ifndef NDEBUG
            ++stats.hits;
#endif
            exact->last_use.store(frame, std::memory_order_relaxed);
            return exact;
        }
        if (render_target) {
            // NAT-113: a target drawn at the same address in the same layout (pitch, tiling,
            // texel size) that is at least as large is drawn into again: GT Sport's dynamic
            // resolution changes target sizes a little from frame to frame (depth 354x150, then
            // 336x150), and the next pass reads what the last one drew there.
            ImageEntry* best = nullptr;
            for (auto it = first; it != last; ++it) {
                const auto& c = it->second->info;
                if (!it->second->from_cpu && it->second->image &&
                    !it->second->reused.load(std::memory_order_relaxed) && c.pitch == info.pitch &&
                    c.tile_mode == info.tile_mode && c.array_mode == info.array_mode &&
                    c.bank_swizzle == info.bank_swizzle && c.alt_tile == info.alt_tile &&
                    c.mips_layout[0].pitch == info.mips_layout[0].pitch &&
                    (c.size != info.size || c.resources != info.resources || SameLayout(c, info)) &&
                    c.num_bits == info.num_bits &&
                    c.num_samples == info.num_samples && c.props.is_depth == info.props.is_depth &&
                    c.props.has_stencil == info.props.has_stencil && c.type == info.type &&
                    c.resources.levels >= info.resources.levels &&
                    c.resources.layers >= info.resources.layers &&
                    CompatibleLayerStrides(c, info) &&
                    c.size.width >= info.size.width && c.size.height >= info.size.height &&
                    (!require_format || c.pixel_format == info.pixel_format) &&
                    (!info.props.is_depth || c.pixel_format == info.pixel_format) &&
                    (!best || u64(c.size.width) * c.size.height <
                                  u64(best->info.size.width) * best->info.size.height)) {
                    best = it->second.get();
                }
            }
            if (best) {
#ifndef NDEBUG
                ++stats.hits;
#endif
#ifndef NDEBUG
                ++stats.reused_larger;
#endif
                best->last_use.store(frame, std::memory_order_relaxed);
                return best;
            }
        }
    }
    if (!create) {
        return nullptr; // OPT-010
    }
    auto entry = std::make_unique<ImageEntry>();
    entry->info = info;
    entry->from_cpu = !render_target;
    entry->made_frame = frame; // FIX-157
    if (image_factory) {
        MadeImage made = image_factory(info, render_target);
        entry->image = std::move(made.image);
        entry->backend_usage = made.usage;
        entry->backend_features = made.features;
    } else {
        entry->image = device.CreateImage(DescOf(info, render_target, true, true));
    }
    if (!entry->image && !image_factory) {
        // Some formats can't be written by shaders, or be render targets; such images are made
        // without those uses.
        entry->image = device.CreateImage(DescOf(info, render_target, true, false));
    }
    if (!entry->image && !image_factory) {
        entry->image = device.CreateImage(DescOf(info, render_target, false, false));
    }
    if (!entry->image) {
        LOG_WARNING(Render_Vulkan, "NAT-020: no image for {:#x} ({}x{}, format {})",
                    info.guest_address, info.size.width, info.size.height,
                    vk::to_string(info.pixel_format));
        return nullptr;
    }
    entry->last_use.store(frame, std::memory_order_relaxed);
    ImageEntry* result;
    {
        std::unique_lock lock{shard.mutex};
        // Another thread may have made it meanwhile.
        const auto [first, last] = shard.images.equal_range(info.guest_address);
        for (auto it = first; it != last; ++it) {
            if (SameLayout(it->second->info, info) &&
                (!require_format || it->second->info.pixel_format == info.pixel_format) &&
                !it->second->reused.load(std::memory_order_relaxed) &&
                (!render_target || !it->second->from_cpu)) {
#ifndef NDEBUG
                ++stats.hits;
#endif
                return it->second.get();
            }
        }
        result = entry.get();
        shard.images.emplace(info.guest_address, std::move(entry));
        const std::pair<VAddr, VAddr> range{info.guest_address,
                                            info.guest_address + std::max<u64>(info.guest_size, 1)};
        NoteChanges({&range, 1}); // OPT-002, OPT-010
    }
    IndexEntry(*result);
#ifndef NDEBUG
    if (WatchedTexture(info.guest_address, info.guest_address + 1)) {
        LOG_WARNING(Render_Vulkan, "FIX-153 watch: made {} {:#x} {}x{} mips {} ({:#x} bytes); "
                    "memory {}", render_target ? "target" : "texture", info.guest_address,
                    info.size.width, info.size.height, info.resources.levels, info.guest_size,
                    WatchedBytes(info.guest_address));
    }
#endif
    if (render_target) {
#ifndef NDEBUG
        ++stats.created_targets;
#endif
        LinkTarget(*result);
        // NAT-127: a new target starts with what memory holds there, as on the console (a pass
        // that draws part of it leaves the rest as it was). GT Sport draws its 16x16 brightness
        // grid into a 64x16 target the CPU reads back for the exposure: the undrawn part went
        // back to memory as uninitialized video memory, and the picture came out too dark.
        if (info.num_samples == 1) {
            result->needs_upload.store(true, std::memory_order_relaxed);
            result->force_upload.store(true, std::memory_order_relaxed);
            std::scoped_lock lock{upload_mutex};
            pending_uploads.push_back(result);
        }
    } else {
#ifndef NDEBUG
        ++stats.created_textures;
#endif
        LinkTexture(*result);
        result->needs_upload.store(true, std::memory_order_relaxed);
        std::scoped_lock lock{upload_mutex};
        pending_uploads.push_back(result);
    }
    return result;
}

void ImageRegistry::NoteChanges(std::span<const std::pair<VAddr, VAddr>> ranges) {
    std::scoped_lock lock{changes_mutex};
    const u64 next = generation.load(std::memory_order_relaxed) + 1;
    for (const auto& [begin, end] : ranges) {
        changes[change_count++ % changes.size()] = {next, begin, end};
    }
    generation.fetch_add(1, std::memory_order_acq_rel);
}

bool ImageRegistry::Untouched(u64 since, std::span<const std::pair<VAddr, VAddr>> ranges) {
    if (generation.load(std::memory_order_acquire) == since) {
        return true;
    }
    std::scoped_lock lock{changes_mutex};
    const u64 kept = std::min<u64>(change_count, changes.size());
    for (u64 i = 0; i < kept; ++i) {
        const Change& change = changes[(change_count - 1 - i) % changes.size()];
        if (change.generation <= since) {
            return true;
        }
        for (const auto& [begin, end] : ranges) {
            if (change.begin < end && change.end > begin) {
                return false;
            }
        }
    }
    // Every change kept is newer: the log reaches back far enough only if none was lost.
    return change_count <= changes.size();
}

bool ImageRegistry::HasImageAt(VAddr address) {
    Shard& shard = ShardOf(address);
    std::shared_lock lock{shard.mutex};
    return shard.images.contains(address);
}

ImageEntry* ImageRegistry::FindExisting(VAddr address,
                                        const std::function<bool(const ImageEntry&)>& accept,
                                        bool prefer_written) {
    Shard& shard = ShardOf(address);
    std::shared_lock lock{shard.mutex};
    const auto [first, last] = shard.images.equal_range(address);
    ImageEntry* best = nullptr;
    for (auto it = first; it != last; ++it) {
        auto& candidate = *it->second;
        if (candidate.reused.load(std::memory_order_relaxed) || !accept(candidate)) {
            continue;
        }
        const auto priority = [&](const ImageEntry& entry) {
            return prefer_written ? entry.write_version.load(std::memory_order_relaxed)
                                  : entry.last_use.load(std::memory_order_relaxed);
        };
        if (!best || priority(candidate) > priority(*best)) {
            best = &candidate;
        }
    }
    return best;
}

SourcePlace PlaceOf(const VideoCore::ImageInfo& target, const VideoCore::ImageInfo& texture) {
    if (target.props.is_depth || texture.props.is_depth || target.props.is_block ||
        texture.props.is_block || target.num_bits != texture.num_bits ||
        target.num_samples != texture.num_samples) {
        return {};
    }
    // A crop: the texture is the corner of the target in the same layout (as IsSubrectOf, any
    // colour format of the texel size).
    if (target.guest_address == texture.guest_address && target.resources.levels == 1 &&
        target.resources.layers == 1 && texture.resources.levels == 1 &&
        texture.resources.layers == 1 && target.pitch == texture.pitch &&
        target.tile_mode == texture.tile_mode && target.bank_swizzle == texture.bank_swizzle &&
        target.alt_tile == texture.alt_tile &&
        target.mips_layout[0].pitch == texture.mips_layout[0].pitch &&
        texture.size.width <= target.size.width && texture.size.height <= target.size.height &&
        (texture.size.width < target.size.width || texture.size.height < target.size.height) &&
        target.size.depth == 1 && texture.size.depth == 1 &&
        target.type == AmdGpu::ImageType::Color2D && texture.type == target.type) {
        return {0, 0, texture.size.width, texture.size.height};
    }
    // A mip or layer of the texture drawn on its own.
    if (const s32 mip = target.MipOf(texture); mip >= 0) {
        const s32 layer = target.SliceOf(texture, mip);
        if (layer < 0 || u32(layer) >= std::max<u32>(texture.resources.layers, 1)) {
            return {};
        }
        return {mip, layer, target.size.width, target.size.height};
    }
    // NAT-131: the texture is one layer (or mip) of the target: a face of a cube drawn as a
    // layered target, read on its own (GT Sport's environment cube, whose faces the cockpit's
    // reflection cube is made of; only face 0, at the target's own address, was found).
    if (const s32 src_mip = texture.MipOf(target); src_mip >= 0) {
        const s32 src_layer = texture.SliceOf(target, src_mip);
        if (src_layer < 0 || u32(src_layer) >= std::max<u32>(target.resources.layers, 1)) {
            return {};
        }
        return {0, 0, texture.size.width, texture.size.height, src_mip, src_layer};
    }
    return {};
}

void ImageRegistry::IndexEntry(ImageEntry& entry) {
    const VAddr base = entry.info.guest_address;
    const u64 span = std::clamp<u64>(entry.info.guest_size, 1, MaxIndexedSize);
    std::unique_lock lock{granule_mutex};
    for (u64 g = base >> GranuleBits; g <= (base + span - 1) >> GranuleBits; ++g) {
        granules[g].push_back(&entry);
    }
}

void ImageRegistry::UnindexEntry(ImageEntry& entry) {
    const VAddr base = entry.info.guest_address;
    const u64 span = std::clamp<u64>(entry.info.guest_size, 1, MaxIndexedSize);
    for (u64 g = base >> GranuleBits; g <= (base + span - 1) >> GranuleBits; ++g) {
        const auto it = granules.find(g);
        if (it == granules.end()) {
            continue;
        }
        auto& list = it->second;
        if (const auto at = std::ranges::find(list, &entry); at != list.end()) {
            *at = list.back();
            list.pop_back();
        }
        if (list.empty()) {
            granules.erase(it);
        }
    }
}

void ImageRegistry::LinkTarget(ImageEntry& target) {
    // NAT-104: textures made before this target over its memory may be made of it.
    const VAddr begin = target.info.guest_address;
    const VAddr end = begin + std::max<u64>(target.info.guest_size, 1);
    ForEachOverlapping(begin, end, [&](ImageEntry& entry) {
        if (!entry.from_cpu) {
            return;
        }
        const auto place = PlaceOf(target.info, entry.info);
#ifndef NDEBUG
        if (WatchedTexture(begin, end)) {
            LOG_WARNING(Render_Vulkan, "FIX-157 watch: new target {:#x} {}x{} layers {} over "
                        "texture {:#x} {}x{} layers {}: {}", begin, target.info.size.width,
                        target.info.size.height, target.info.resources.layers,
                        entry.info.guest_address, entry.info.size.width, entry.info.size.height,
                        entry.info.resources.layers, place.mip >= 0 ? "linked" : "no place");
        }
#endif
        if (place.mip >= 0) {
            {
                std::scoped_lock sources_lock{entry.sources_mutex};
                entry.sources.push_back({&target, place});
            }
            entry.sources_dirty.store(true, std::memory_order_relaxed);
            std::scoped_lock target_lock{target.sources_mutex};
            target.dependents.push_back(&entry);
        }
    });
}

void ImageRegistry::LinkTexture(ImageEntry& texture) {
    // NAT-104: the render targets this texture is made of. NAT-041: a texture over a target
    // of another kind of layout reads what the CPU last wrote there, not the picture; logged
    // (a few times per layout pair) to learn which of these GT Sport has.
    const VAddr begin = texture.info.guest_address;
    const VAddr end = begin + std::max<u64>(texture.info.guest_size, 1);
    ForEachOverlapping(begin, end, [&](ImageEntry& other) {
        const ImageEntry* entry = &other;
        {
            if (entry->from_cpu || entry->reused.load(std::memory_order_relaxed)) {
                return;
            }
            const auto place = PlaceOf(entry->info, texture.info);
#ifndef NDEBUG
            if (WatchedTexture(begin, end)) {
                LOG_WARNING(Render_Vulkan, "FIX-157 watch: new texture {:#x} {}x{} layers {} "
                            "over target {:#x} {}x{} layers {}: {}", begin,
                            texture.info.size.width, texture.info.size.height,
                            texture.info.resources.layers, entry->info.guest_address,
                            entry->info.size.width, entry->info.size.height,
                            entry->info.resources.layers, place.mip >= 0 ? "linked" : "no place");
            }
#endif
            if (place.mip >= 0) {
                {
                    std::scoped_lock sources_lock{texture.sources_mutex};
                    texture.sources.push_back({&other, place});
                }
                texture.sources_dirty.store(true, std::memory_order_relaxed);
                std::scoped_lock target_lock{other.sources_mutex};
                other.dependents.push_back(&texture);
                return;
            }
#ifndef NDEBUG
            const auto& a = texture.info;
            const auto& b = entry->info;
            const u64 key = (a.guest_address * 31 + b.guest_address) ^ (u64(a.size.width) << 40) ^
                            (u64(b.size.width) << 20) ^ (u64(a.resources.levels) << 8) ^
                            u64(b.resources.levels);
            std::scoped_lock log_lock{overlap_log_mutex};
            if (overlap_logged.size() < 4096 && overlap_logged.insert(key).second) {
                LOG_WARNING(Render_Vulkan,
                            "NAT-041: texture {:#x}+{:#x} {}x{} mips {} layers {} tile {} {} bits "
                            "over render target {:#x}+{:#x} {}x{} mips {} layers {} tile {} {} "
                            "bits{}",
                            a.guest_address, a.guest_size, a.size.width, a.size.height,
                            a.resources.levels, a.resources.layers, u32(a.tile_mode), a.num_bits,
                            b.guest_address, b.guest_size, b.size.width, b.size.height,
                            b.resources.levels, b.resources.layers, u32(b.tile_mode), b.num_bits,
                            b.props.is_depth ? " (depth)" : "");
            }
#endif
        }
    });
}

void ImageRegistry::ForEachOverlapping(VAddr begin, VAddr end,
                                       const std::function<void(ImageEntry&)>& fn) {
    if (end <= begin) {
        return;
    }
    // Each image once: in the first granule of the range it covers.
    const u64 first = begin >> GranuleBits;
    const u64 last = (end - 1) >> GranuleBits;
    const auto visit = [&](u64 g, const std::vector<ImageEntry*>& list) {
        for (ImageEntry* entry : list) {
            const VAddr base = entry->info.guest_address;
            const u64 span = std::clamp<u64>(entry->info.guest_size, 1, MaxIndexedSize);
            if (g == std::max(first, base >> GranuleBits) && base < end && base + span > begin) {
                fn(*entry);
            }
        }
    };
    std::shared_lock lock{granule_mutex};
    if (last - first >= granules.size()) {
        for (const auto& [g, list] : granules) {
            if (g >= first && g <= last) {
                visit(g, list);
            }
        }
        return;
    }
    for (u64 g = first; g <= last; ++g) {
        if (const auto it = granules.find(g); it != granules.end()) {
            visit(g, it->second);
        }
    }
}

std::vector<std::pair<VAddr, u32>> ImageRegistry::Crowded(u32 count) {
    std::unordered_map<VAddr, u32> per_address;
    for (Shard& shard : shards) {
        std::shared_lock lock{shard.mutex};
        for (const auto& [base, entry] : shard.images) {
            ++per_address[base];
        }
    }
    std::vector<std::pair<VAddr, u32>> list(per_address.begin(), per_address.end());
    std::ranges::sort(list, std::greater{}, [](const auto& item) { return item.second; });
    list.resize(std::min<size_t>(list.size(), count));
    return list;
}

u64 ImageRegistry::Count() {
    u64 count = 0;
    for (Shard& shard : shards) {
        std::shared_lock lock{shard.mutex};
        count += shard.images.size();
    }
    return count;
}

u32 ImageRegistry::Evict(u64 before, const std::function<void(ImageEntry&)>& on_evict,
                         const std::function<bool(ImageEntry&)>& keep, u32 max_retired) {
    if (max_retired == 0) return 0;
    std::unordered_set<const ImageEntry*> gone;
    std::vector<ImageEntry*> candidates;
    const bool bounded = max_retired != ~u32{0};
    const u32 first = bounded ? eviction_shard_cursor : 0;
    // Scan metadata in every shard: a heavily populated single shard must still
    // be able to retire a full budget every frame, rather than once per rotation.
    // Rotate the selection order so a busy shard cannot starve the others.
    const u32 visits = NumShards;
    if (bounded) eviction_shard_cursor = (first + 1) % NumShards;
    for (u32 i = 0; i < visits; ++i) {
        Shard& shard = shards[(first + i) % NumShards];
        std::shared_lock lock{shard.mutex};
        for (const auto& [base, entry] : shard.images) {
            if (entry->last_use.load(std::memory_order_relaxed) < before) {
                candidates.push_back(entry.get());
            }
        }
    }
    // FIX-157: pictures the caller keeps stay (asked with no lock held: it looks around).
    for (ImageEntry* entry : candidates) {
        if (gone.size() >= max_retired) break;
        if (!keep || !keep(*entry)) {
            gone.insert(entry);
        }
    }
    if (gone.empty()) {
        return 0;
    }
    // Out of the shards first: from then on nothing finds them (a CPU write fault on another
    // thread queues the textures it finds for upload while holding a shard's lock).
    std::vector<std::unique_ptr<ImageEntry>> taken;
    for (u32 i = 0; i < visits; ++i) {
        Shard& shard = shards[(first + i) % NumShards];
        std::unique_lock lock{shard.mutex};
        for (auto it = shard.images.begin(); it != shard.images.end();) {
            // OPT-010: one a job thread found since the scan stays.
            if (gone.contains(it->second.get()) &&
                it->second->last_use.load(std::memory_order_relaxed) < before) {
                taken.push_back(std::move(it->second));
                it = shard.images.erase(it);
            } else {
                ++it;
            }
        }
    }
    gone.clear();
    for (const auto& entry : taken) {
        gone.insert(entry.get());
    }
    {
        std::unique_lock lock{granule_mutex};
        for (const auto& entry : taken) {
            UnindexEntry(*entry);
        }
    }
    {
        std::scoped_lock lock{upload_mutex};
        std::erase_if(pending_uploads, [&](ImageEntry* entry) { return gone.contains(entry); });
    }
    if (!taken.empty()) {
        std::vector<std::pair<VAddr, VAddr>> ranges;
        ranges.reserve(taken.size());
        for (const auto& entry : taken) {
            ranges.emplace_back(entry->info.guest_address,
                                entry->info.guest_address + std::max<u64>(entry->info.guest_size, 1));
        }
        NoteChanges(ranges); // OPT-002, OPT-010
    }
    // The links between textures and targets that went (NAT-111: both directions).
    for (const auto& entry : taken) {
        std::vector<ImageEntry*> others;
        {
            std::scoped_lock lock{entry->sources_mutex};
            for (const auto& link : entry->sources) {
                others.push_back(link.source);
            }
            others.insert(others.end(), entry->dependents.begin(), entry->dependents.end());
        }
        for (ImageEntry* other : others) {
            if (gone.contains(other)) {
                continue;
            }
            std::scoped_lock lock{other->sources_mutex};
            std::erase_if(other->sources,
                          [&](const SourceLink& link) { return link.source == entry.get(); });
            std::erase(other->dependents, entry.get());
        }
    }
    for (const auto& entry : taken) {
        on_evict(*entry);
    }
#ifndef NDEBUG
    stats.evicted += taken.size();
#endif
    return static_cast<u32>(taken.size());
}

void ImageRegistry::MarkStale(ImageEntry& entry, Stale why) {
#ifndef NDEBUG
    ++stats.stale[static_cast<u32>(why)];
#endif
#ifndef NDEBUG
    if (WatchedTexture(entry.info.guest_address, entry.info.guest_address + 1)) {
        LOG_WARNING(Render_Vulkan, "FIX-153 watch: {:#x} stale ({}), pending before {}; memory {}",
                    entry.info.guest_address,
                    why == Stale::CpuWrite ? "CPU write" : why == Stale::Unmap ? "unmap" : "GPU write",
                    entry.needs_upload.load(), WatchedBytes(entry.info.guest_address));
    }
#endif
    if (why == Stale::GpuWrite) {
        entry.gpu_backing_written.store(true, std::memory_order_release);
    }
    if (why != Stale::CpuWrite) {
        entry.force_upload.store(true, std::memory_order_relaxed);
    }
    const bool already_dirty = entry.needs_upload.exchange(true);
#ifndef NDEBUG
    const u32 prior_uploads = entry.total_uploads.load(std::memory_order_relaxed);
    if (!already_dirty && entry.from_cpu && entry.info.guest_size >= (4ull << 20) &&
        entry.info.size.width >= 2048 && (prior_uploads <= 3 || prior_uploads % 16 == 0)) {
        static std::atomic<u32> logged{};
        if (logged.fetch_add(1,std::memory_order_relaxed) < 4096) {
            LOG_WARNING(Render_Vulkan,
                "UPLOAD-CAUSE: dirty image={:#x} guest={:#x} bytes={} prior={} reason={} forced={}",
                entry.image->NativeHandle(),entry.info.guest_address,entry.info.guest_size,
                prior_uploads,static_cast<u32>(why),entry.force_upload.load());
        }
    }
#endif
    // All large CPU-texture invalidations wait for actual image use. In particular,
    // OnUnmap runs before guest memory is released: eagerly queuing here re-imports
    // and uploads a discarded texture while the unmapping thread waits for us.
    // Preserve dirty/force flags so a later use after remapping still refreshes it.
    // Explicit RequestUpload separately admits its required rewrite immediately.
    const bool defer = entry.from_cpu && entry.info.guest_size >= (4ull << 20);
    if (!already_dirty && !defer) {
        std::scoped_lock upload_lock{upload_mutex};
        pending_uploads.push_back(&entry);
    }
}

void ImageRegistry::Invalidate(VAddr address, u64 size, Stale why) {
    ForEachOverlapping(address, address + size, [&](ImageEntry& entry) {
        if (entry.from_cpu) {
            MarkStale(entry, why);
        } else {
            entry.guest_modified.store(true, std::memory_order_seq_cst);
            // Unmapping ends the allocation's lifetime, even before it was kept.
            // Retire it from lookup without destroying resources still in flight.
            if (why == Stale::Unmap || entry.kept.load(std::memory_order_seq_cst)) {
                MarkReused(entry); // FIX-157
            }
        }
    });
}

void ImageRegistry::MarkReused(ImageEntry& entry) {
    if (entry.reused.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    // What Find returned for the range may change (OPT-002 memos look again).
    const std::pair<VAddr, VAddr> range{entry.info.guest_address,
                                        entry.info.guest_address +
                                            std::max<u64>(entry.info.guest_size, 1)};
    NoteChanges({&range, 1});
#ifndef NDEBUG
    if (WatchedTexture(entry.info.guest_address, entry.info.guest_address + 1)) {
        LOG_WARNING(Render_Vulkan, "FIX-157 watch: kept {:#x} reused", entry.info.guest_address);
    }
#endif
}

void ImageRegistry::ForeignWrite(ImageEntry& entry, u64 size, bool exact, u64 clock) {
    // NAT-129: a storage buffer much larger than the picture is the scratch pool GT Sport
    // binds whole: what it wrote is unknown, and it doesn't count.
    if (!exact && size > 2 * std::max<u64>(entry.info.guest_size, 1)) {
        return;
    }
    u64 seen = entry.foreign_clock.load(std::memory_order_relaxed);
    while (seen < clock &&
           !entry.foreign_clock.compare_exchange_weak(seen, clock, std::memory_order_relaxed)) {
    }
    if (entry.kept.load(std::memory_order_relaxed)) {
        MarkReused(entry);
    }
}

bool ImageRegistry::MemoryUnchanged(const ImageEntry& entry) {
    const u64 size = entry.info.guest_size;
    if (entry.content_hash == 0 || size == 0 || !memory_clamp ||
        memory_clamp(entry.info.guest_address, size) != size) {
        return false;
    }
    // FIX-160: read safely; the game can release the memory while the caches still say it is
    // mapped (a GPU-thread fault under Proton).
    const u64 hash = Core::HashGuestBytes(entry.info.guest_address, size) | 1;
    return hash == entry.content_hash;
}

void ImageRegistry::GpuWrote(VAddr address, u64 size, const std::array<u32, 2>* pattern,
                             bool exact, u64 clock) {
#ifndef NDEBUG
    if (WatchedTexture(address, address + std::max<u64>(size, 1)) && WatchSample()) {
        static const bool dump_on_fill = std::getenv("RYGT_FRAME_DUMP_ON_FILL") != nullptr;
        if (pattern && dump_on_fill) {
            g_dump_request = true; // FIX-156 (diagnosis): dump the next frame
        }
        LOG_WARNING(Render_Vulkan, "FIX-153 watch: GPU {} {:#x}+{:#x} pattern {:#x} {:#x}",
                    pattern ? "fill" : exact ? "copy" : "storage write", address, size,
                    pattern ? (*pattern)[0] : 0u, pattern ? (*pattern)[1] : 0u);
    }
#endif
    ForEachOverlapping(address, address + std::max<u64>(size, 1), [&](ImageEntry& entry) {
        if (entry.from_cpu) {
            // NAT-129: a storage buffer as big as GT Sport's scratch pool would upload every
            // texture in it again at each dispatch (10-30 GB in 10 s): only buffers about the
            // texture's size count, as for targets below.
            if (!exact && size > 2 * std::max<u64>(entry.info.guest_size, 1)) {
                return;
            }
            // Retain ownership even after clears/source links/force flags are consumed.
            entry.gpu_backing_written.store(true, std::memory_order_release);
            // NAT-133: a fill that covers the whole texture fills it on the GPU at its next use
            // (GT Sport clears its environment cube and motion targets with fills, and textures
            // over that memory were uploaded again 60 times a second).
            if (pattern && address <= entry.info.guest_address &&
                address + size >= entry.info.guest_address + entry.info.guest_size) {
                entry.rewrite = ImageEntry::Rewrite::Clear;
                entry.rewrite_pattern = *pattern;
                entry.rewrite_bytes = entry.info.guest_size;
                entry.rewrite_clock = clock;
                return;
            }
            bool made_of_targets;
            {
                std::scoped_lock lock{entry.sources_mutex};
                made_of_targets = !entry.sources.empty();
            }
            if (!made_of_targets && entry.info.num_samples == 1) {
#ifndef NDEBUG
                const u32 prior = entry.total_uploads.load(std::memory_order_relaxed);
                if (!entry.needs_upload.load() && entry.info.guest_size >= (4ull << 20) &&
                    entry.info.size.width >= 2048 && (prior <= 3 || prior % 16 == 0)) {
                    static std::atomic<u32> logged{};
                    if (logged.fetch_add(1,std::memory_order_relaxed) < 4096) {
                        u64 sequence = 0, kind = 0, program = 0;
                        bool stable = false;
                        if (GpuCheckpoints::supported.load(std::memory_order_relaxed)) {
                            sequence = GpuCheckpoints::sequence.load(std::memory_order_acquire);
                            const auto& slot = GpuCheckpoints::Slots()[sequence % GpuCheckpoints::SlotCount];
                            const u64 before = slot.stamp.load(std::memory_order_acquire);
                            kind = slot.kind.load(std::memory_order_relaxed);
                            program = slot.program.load(std::memory_order_relaxed);
                            stable = before == sequence && sequence != 0 &&
                                slot.stamp.load(std::memory_order_acquire) == before;
                        }
                        LOG_WARNING(Render_Vulkan,
                            "UPLOAD-CAUSE: gpu-write image={:#x} guest={:#x} prior={} write={:#x}+{:#x} exact={} fill={} clock={} preceding-checkpoint={} preceding-kind={} preceding-program={:#x} stable={}",
                            entry.image->NativeHandle(),entry.info.guest_address,prior,address,size,
                            exact,pattern != nullptr,clock,sequence,kind,program,stable);
                    }
                }
#endif
                MarkStale(entry, Stale::GpuWrite);
#ifndef NDEBUG
                static std::atomic<u32> logged{};
                if (const u32 n = logged++; n < 40 || n % 2000 == 0) {
                    LOG_WARNING(Render_Vulkan,
                                "NAT-129: texture {:#x} {}x{} ({:#x} bytes) is uploaded again: a "
                                "{} wrote {:#x}+{:#x} ({} so far)",
                                entry.info.guest_address, entry.info.size.width,
                                entry.info.size.height, entry.info.guest_size,
                                exact ? (pattern ? "fill" : "copy") : "storage buffer", address,
                                size, n + 1);
                }
#endif
            }
            return;
        }
        // FIX-152: a fill that covers a whole target clears it wherever it starts. GT Sport
        // clears the car showroom's six environment cube faces (six targets 256 KB apart) with
        // one fill from the first face: only that one was cleared, the others added the studio's
        // lights frame after frame, and the auto exposure made the showroom screens black.
        const bool covered = pattern && address <= entry.info.guest_address &&
                             address + size >= entry.info.guest_address + entry.info.guest_size;
        if (entry.info.guest_address != address && !covered) {
            ForeignWrite(entry, size, exact, clock); // FIX-157
            return;
        }
#ifndef NDEBUG
        if (WatchedTexture(entry.info.guest_address, entry.info.guest_address + 1) &&
            WatchSample()) {
            LOG_WARNING(Render_Vulkan, "FIX-153 watch: target {:#x} ({:#x} bytes, {} layers) "
                        "written {:#x}+{:#x}: {}", entry.info.guest_address, entry.info.guest_size,
                        entry.info.resources.layers, address, size,
                        pattern ? (covered ? "clear (covered)" : "clear (partial)")
                        : size >= entry.info.guest_size && (exact || size <= 2 * entry.info.guest_size)
                            ? "upload"
                            : "ignored");
        }
#endif
        if (pattern) {
#ifndef NDEBUG
            ++stats.targets_rewritten;
#endif
            entry.rewrite = ImageEntry::Rewrite::Clear;
            entry.rewrite_pattern = *pattern;
            entry.rewrite_bytes =
                covered ? entry.info.guest_size : std::min<u64>(size, entry.info.guest_size);
            entry.rewrite_clock = clock;
        } else if (size >= entry.info.guest_size &&
                   (exact || size <= 2 * entry.info.guest_size)) {
#ifndef NDEBUG
            ++stats.targets_rewritten;
#endif
            entry.rewrite = ImageEntry::Rewrite::Upload;
            entry.rewrite_bytes = entry.info.guest_size;
            entry.rewrite_clock = clock;
#ifndef NDEBUG
            static std::atomic<u32> logged{};
            if (const u32 n = logged++; n < 40 || n % 2000 == 0) {
                LOG_WARNING(Render_Vulkan,
                            "NAT-129: target {:#x} {}x{} ({:#x} bytes) is uploaded again: a {} "
                            "wrote {:#x}+{:#x} ({} so far)",
                            entry.info.guest_address, entry.info.size.width,
                            entry.info.size.height, entry.info.guest_size,
                            exact ? "copy" : "storage buffer", address, size, n + 1);
            }
#endif
        } else {
            ForeignWrite(entry, size, exact, clock); // FIX-157
        }
    });
}

void ImageRegistry::CpuWrite(VAddr address, u64 size, u64 exact) {
#ifndef NDEBUG
    if (WatchedTexture(address, address + size)) {
        LOG_WARNING(Render_Vulkan, "FIX-153 watch: CPU write {:#x}+{:#x} (exact {:#x})", address,
                    size, exact);
    }
#endif
    constexpr u64 HashedSize = 4ull << 20;
    const VAddr written_end = address + (exact != 0 ? exact : size);
    ForEachOverlapping(address, address + size, [&](ImageEntry& entry) {
        if (!entry.from_cpu) {
            entry.guest_modified.store(true, std::memory_order_seq_cst);
            if (entry.kept.load(std::memory_order_seq_cst)) {
                MarkReused(entry); // FIX-157
            }
            return;
        }
        const VAddr base = entry.info.guest_address;
        const bool hit = base < written_end && base + entry.info.guest_size > address;
        // A small texture is checked by hash at its next use (its page is no longer watched,
        // so later writes there go unseen); a large one is uploaded again only when written.
        if (hit || entry.info.guest_size <= HashedSize) {
#ifndef NDEBUG
            const auto fault = UploadCause::cpu_fault;
            const u32 prior = entry.total_uploads.load(std::memory_order_relaxed);
            if (!entry.needs_upload.load() && entry.info.guest_size >= (4ull << 20) &&
                entry.info.size.width >= 2048 && (prior <= 3 || prior % 16 == 0)) {
                static std::atomic<u32> logged{};
                if (logged.fetch_add(1,std::memory_order_relaxed) < 4096) {
                    LOG_WARNING(Render_Vulkan,
                        "UPLOAD-CAUSE: cpu-fault image={:#x} guest={:#x} bytes={} prior={} fault={:#x} reported-size={} exact-size={} page={:#x}+{:#x} context={}",
                        entry.image->NativeHandle(),entry.info.guest_address,entry.info.guest_size,
                        prior,fault.address,fault.size,fault.exact,address,size,fault.active);
                }
            }
#endif
            MarkStale(entry, Stale::CpuWrite);
        }
    });
}

std::vector<Rhi::BufferImageCopy> MipCopies(const VideoCore::ImageInfo& info) {
    std::vector<Rhi::BufferImageCopy> copies;
    for (u32 m = 0; m < info.resources.levels; ++m) {
        const u32 width = std::max(info.size.width >> m, 1u);
        const u32 height = std::max(info.size.height >> m, 1u);
        const u32 depth = info.props.is_volume ? std::max(info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = info.mips_layout[m];
        copies.push_back({
            .buffer_offset = mip_offset,
            .buffer_row_texels = mip_pitch,
            .buffer_image_rows = mip_height,
            .mip = m,
            .base_layer = 0,
            .layers = info.resources.layers,
            .extent = {mip_pitch ? std::min<u32>(mip_pitch, width) : width,
                       mip_height ? std::min<u32>(mip_height, height) : height, depth},
            .aspect = static_cast<u8>(info.props.is_depth ? Rhi::AspectDepth : Rhi::AspectColor),
        });
    }
    return copies;
}

std::unique_ptr<Rhi::Buffer> ImageRegistry::TilingParamsBuffer(const VideoCore::ImageInfo& info) {
    auto buffer = device.CreateBuffer({.size = sizeof(TilingParams),
                                       .usage = Rhi::BufferUsageUniform,
                                       .memory = Rhi::MemoryKind::Upload});
    if (!buffer || !buffer->Mapped()) {
        return nullptr;
    }
    TilingParams params{};
    params.bank_swizzle = info.bank_swizzle;
    params.micro_tiled_mips = info.micro_tiled_mips;
    params.num_mips = info.resources.levels;
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max<u16>((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max<u16>((mip_info.height + 3) / 4, 1U);
        }
    }
    std::memcpy(buffer->Mapped(), &params, sizeof(params));
    return buffer;
}

const Rhi::Pipeline* ImageRegistry::TilingPipeline(const VideoCore::ImageInfo& info, bool tiler) {
  const u64 key = VideoCore::TilingPipelineKey(info, tiler);
  std::scoped_lock lock{untile_mutex};
  if (const auto it = untile_pipelines.find(key);
      it != untile_pipelines.end()) {
    return it->second.get();
  }
    const bool macro = AmdGpu::IsMacroTiled(info.array_mode);
    const auto values = VideoCore::TilingSpecialization(info, tiler);
    std::array<Rhi::SpecConstant, 12> constants{};
    for (u32 i = 0; i < constants.size(); ++i) {
        constants[i].id = i;
        constants[i].value = values[i];
    }
    const auto code = UntileCode(info.num_bits, macro);
    if (code.empty() || !untile_layout) {
        return nullptr;
    }
    auto pipeline = device.CreateComputePipeline({
        .layout = untile_layout.get(),
        .shader = {Rhi::ShaderStage::Compute, code, "main",
                   std::span<const Rhi::SpecConstant>{constants.data(), macro ? 12u : 4u}},
    });
    const Rhi::Pipeline* result = pipeline.get();
    untile_pipelines.emplace(key, std::move(pipeline));
    return result;
}

Rhi::Buffer* ImageRegistry::AcquireUploadGatherScratch(Rhi::CommandList& list, u64 size) {
    const u64 capacity = UploadScratchCapacity(size);
    if (!capacity) return nullptr;
    if (!upload_gather_scratch || upload_gather_scratch->Size() < size) {
        auto larger = device.CreateBuffer({.size = capacity,
            .usage = Rhi::BufferUsageStorage | Rhi::BufferUsageTransferSrc |
                     Rhi::BufferUsageTransferDst,
            .memory = Rhi::MemoryKind::DeviceLocal, .name = "shared native upload gather"});
        if (!larger) return nullptr;
        // Buffer destruction uses the device's pending-submission retirement tick.
        upload_gather_scratch = std::move(larger);
    }
    // Finish the previous upload's reads before overwriting the gathered bytes.
    list.Barrier(Rhi::ScopeCompute | Rhi::ScopeTransfer, Rhi::ScopeTransfer);
    return upload_gather_scratch.get();
}

u32 ImageRegistry::RecordUploads(Rhi::CommandList& list, bool trace_slow_calls) {
    SampleGuestHashCalls hash_sampling{trace_slow_calls};
    SlowCallTrace<16> trace{trace_slow_calls, "image-uploads",
        {"queue-lock", "validate", "watch-pages", "hash", "gpu-source", "cpu-allocate",
         "cpu-copy", "tiling-pipeline", "tiling-params", "scratch", "barrier-bindings",
         "dispatch", "mip-copies", "copy-image", "final-barrier", "retire"}};
    std::vector<ImageEntry*> pending;
    {
        std::scoped_lock lock{upload_mutex};
        pending.swap(pending_uploads);
    }
    // Duplicates may observe needs_upload becoming true again while an earlier
    // snapshot is copied. Consume each image only once, preserving that new dirty
    // state for the next actual use rather than uploading it repeatedly now.
    UniqueUploadBatch(pending);
    u32 uploaded = 0;
    std::vector<std::unique_ptr<Rhi::Buffer>> staging; // freed after the GPU used them
    for (ImageEntry* entry : pending) {
        if (!entry->needs_upload.exchange(false)) {
            continue;
        }
        const VideoCore::ImageInfo& info = entry->info;
        const u64 size = info.guest_size;
#ifndef NDEBUG
        GpuCheckpoints::Record(reinterpret_cast<VkCommandBuffer>(list.NativeHandle()),
                               3, entry->last_use.load(std::memory_order_relaxed),
                               info.guest_address, size, u64(info.size.width) << 32 | info.size.height);
#endif
        trace.Next(1, info.guest_address, size);
        if (size == 0) {
            continue;
        }
        if (info.num_samples > 1) {
            continue;
        }
        // The guest may unmap a valid image after it was queued. Recheck before
        // allocating or importing its complete span; CPU-readable is not required.
        if (backing_validator && !backing_validator(info.guest_address, size)) {
            static std::atomic<u32> logged{};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
                LOG_WARNING(Render_Vulkan,
                    "IMAGE-BACKING: cancelled unmapped queued image {:#x}+{:#x} {}x{} pitch {} mips {} layers {}",
                    info.guest_address, size, info.size.width, info.size.height,
                    info.pitch, info.resources.levels, info.resources.layers);
            }
            continue;
        }
        trace.Next(2);
        if (upload_hook && entry->from_cpu) {
            upload_hook(*entry); // NAT-105: CPU writes from here on are seen
        }
        // NAT-109: a small texture whose bytes did not change (a write elsewhere on its page)
        // is not uploaded again; GPU writes to its memory always upload.
        trace.Next(3);
        constexpr u64 HashedSize = 4ull << 20;
        const bool forced = entry->force_upload.exchange(false);
        // FIX-157: render targets of any size (what their memory held when made).
        if ((size <= HashedSize || !entry->from_cpu) && memory_clamp &&
            memory_clamp(info.guest_address, size) == size) {
            // FIX-160: a safe read (see MemoryUnchanged).
            const u64 hash = Core::HashGuestBytes(info.guest_address, size) | 1;
            if (!forced && hash == entry->content_hash) {
#ifndef NDEBUG
                ++upload_stats.unchanged;
#endif
#ifndef NDEBUG
                if (WatchedTexture(info.guest_address, info.guest_address + 1)) {
                    LOG_WARNING(Render_Vulkan, "FIX-153 watch: {:#x} unchanged ({:016x}), not "
                                "uploaded", info.guest_address, hash);
                }
#endif
                continue;
            }
            entry->content_hash = hash;
        } else {
            entry->content_hash = 0;
        }
        // NAT-107: the bytes as the GPU sees guest memory, in GPU order (textures the game's
        // GPU work filled, in memory the CPU may not even read), else a copy the CPU makes now.
        trace.Next(4);
        UploadSource gpu_source;
        if (upload_source) {
            gpu_source = upload_source(*entry, list);
        }
        if (entry->total_uploads.load(std::memory_order_relaxed) == 0) {
            entry->guest_writes_tracked = gpu_source.guest_writes_tracked;
        }
        std::unique_ptr<Rhi::Buffer> source;
        const Rhi::Buffer* bytes = gpu_source.buffer;
        u64 bytes_offset = gpu_source.offset;
        for (auto& keep : gpu_source.keep) {
            staging.push_back(std::move(keep));
        }
#ifndef NDEBUG
        if (bytes) {
            if (gpu_source.kind == 4) ++upload_stats.cpu;
            else ++upload_stats.gpu;
        } else {
            ++upload_stats.cpu;
        }
#endif
        if (!bytes) {
            trace.Next(5);
            source = device.CreateBuffer({.size = size,
                                          .usage = Rhi::BufferUsageStorage |
                                                   Rhi::BufferUsageTransferSrc,
                                          .memory = Rhi::MemoryKind::Upload});
            if (!source || !source->Mapped()) {
                continue;
            }
            // Only what is mapped is read (a T# can reach past the end of a mapping); the rest of
            // the picture is zeros.
            trace.Next(6);
            const u64 readable = memory_clamp ? memory_clamp(info.guest_address, size) : size;
            if (readable < size) {
                std::memset(source->Mapped() + readable, 0, size - readable);
                static std::atomic<u32> logged{};
                if (logged++ < 16) {
                    LOG_WARNING(Render_Vulkan,
                                "NAT-042: texture {:#x}+{:#x} ({}x{}, {} mips) is mapped for {:#x} "
                                "bytes only; the rest uploads as zeros",
                                info.guest_address, size, info.size.width, info.size.height,
                                info.resources.levels, readable);
                }
            }
            // FIX-160: the game can release part of it meanwhile; then zeros where nothing is
            // mapped any more.
            if (readable != 0 &&
                !Core::RytyMemory::GuardedCopy(source->Mapped(),
                                               reinterpret_cast<const void*>(info.guest_address),
                                               readable)) {
                Core::Memory::Instance()->CopySparseMemory(info.guest_address, source->Mapped(),
                                                           readable);
            }
            bytes = source.get();
            bytes_offset = 0;
        }
        const Rhi::Buffer* linear = bytes;
        u64 linear_offset = bytes_offset;
        if (info.props.is_tiled) {
            trace.Next(7);
            const Rhi::Pipeline* untile = TilingPipeline(info, false);
            // Immutable per-image parameters are shared by uploads and downloads.
            // Untiling scratch is shared across uploads, including the observed 147 MiB path.
            trace.Next(8);
            if (!entry->download_params) {
                entry->download_params = TilingParamsBuffer(info);
            }
            const Rhi::Buffer* params_buffer = entry->download_params.get();
            std::unique_ptr<Rhi::Buffer> temporary;
            trace.Next(9);
            const u64 capacity = UploadScratchCapacity(size);
            if (capacity && (!upload_scratch || upload_scratch->Size() < size)) {
                auto larger = device.CreateBuffer({.size = capacity,
                    .usage = Rhi::BufferUsageStorage | Rhi::BufferUsageTransferSrc,
                    .memory = Rhi::MemoryKind::DeviceLocal, .name = "shared upload untile scratch"});
                if (larger) {
                    upload_scratch = std::move(larger);
#ifndef NDEBUG
                    ++upload_stats.scratch_created;
#endif
                }
            }
            Rhi::Buffer* out = nullptr;
            if (capacity && upload_scratch && upload_scratch->Size() >= size) {
                out = upload_scratch.get();
#ifndef NDEBUG
                ++upload_stats.scratch_reused;
#endif
            } else {
                temporary = device.CreateBuffer({.size = size,
                    .usage = Rhi::BufferUsageStorage | Rhi::BufferUsageTransferSrc,
                    .memory = Rhi::MemoryKind::DeviceLocal});
                out = temporary.get();
#ifndef NDEBUG
                ++upload_stats.scratch_transient;
#endif
            }
            if (!untile || !params_buffer || !out) {
                continue;
            }
            // WAR dependency: the preceding copy must finish reading shared scratch
            // before this dispatch overwrites it. All uses are on the recording queue.
            trace.Next(10);
            if (!temporary) {
                list.Barrier(Rhi::ScopeTransfer | Rhi::ScopeCompute, Rhi::ScopeCompute);
            }
            list.BindPipeline(untile);
            const std::array<Rhi::Binding, 3> bindings{{
                {.slot = 0, .type = Rhi::BindingType::StorageBuffer, .buffer = bytes,
                 .offset = bytes_offset, .size = size},
                {.slot = 1, .type = Rhi::BindingType::StorageBuffer, .buffer = out, .size = size},
                {.slot = 2, .type = Rhi::BindingType::UniformBuffer, .buffer = params_buffer,
                 .size = sizeof(TilingParams)},
            }};
            list.PushBindings(bindings);
            trace.Next(11);
            list.Dispatch(static_cast<u32>((size / (info.num_bits / 8)) / 64), 1, 1);
            list.Barrier(Rhi::ScopeCompute, Rhi::ScopeTransfer);
            linear = out;
            linear_offset = 0;
            if (temporary) staging.push_back(std::move(temporary));
        }
        trace.Next(12);
        auto copies = MipCopies(info);
        for (auto& copy : copies) {
            copy.buffer_offset += linear_offset;
        }
        trace.Next(13);
        list.CopyBufferToImage(linear, entry->image.get(), copies);
#ifndef NDEBUG
        const u8 kind = source ? 4 : gpu_source.kind; // FIX-153 (diagnosis)
#endif
        if (source) {
            staging.push_back(std::move(source));
        }
#ifndef NDEBUG
        stats.upload_bytes += size;
#endif
#ifndef NDEBUG
        if (WatchedTexture(info.guest_address, info.guest_address + 1)) {
            LOG_WARNING(Render_Vulkan, "FIX-153 watch: {:#x} uploaded (kind {}, forced {}, hash "
                        "{:016x}); memory {}", info.guest_address, kind,
                        forced, entry->content_hash, WatchedBytes(info.guest_address));
        }
#endif
#ifndef NDEBUG
        entry->upload_kind = kind;
        const u32 prior = entry->total_uploads.load(std::memory_order_relaxed);
        if (entry->from_cpu && size >= (4ull << 20) && info.size.width >= 2048 &&
            (prior <= 3 || prior % 16 == 0)) {
            static std::atomic<u32> logged{};
            if (logged.fetch_add(1,std::memory_order_relaxed) < 4096) {
                LOG_WARNING(Render_Vulkan,
                    "UPLOAD-CAUSE: upload image={:#x} guest={:#x} bytes={} prior={} forced={} source-kind={} hash={:016x} frame={} total-mips={} width={} height={}",
                    entry->image->NativeHandle(),info.guest_address,size,prior,forced,kind,
                    entry->content_hash,entry->last_use.load(),info.resources.levels,
                    info.size.width,info.size.height);
            }
        }
#endif
        ++entry->total_uploads;
#ifndef NDEBUG
        ++entry->upload_count;
#endif
        ++uploaded;
    }
    trace.Next(14, 0, pending.size());
    if (uploaded) {
        list.Barrier(Rhi::ScopeTransfer, Rhi::ScopeDraw | Rhi::ScopeCompute);
#ifndef NDEBUG
        stats.uploads += uploaded;
#endif
    }
    // The device frees these once the submission that runs this list has finished.
    trace.Next(15);
    staging.clear();
    return uploaded;
}

bool ImageRegistry::RecordDownload(Rhi::CommandList& list, ImageEntry& entry, const Rhi::Buffer& out,
                                   u64 out_offset) {
    const VideoCore::ImageInfo& info = entry.info;
    const u64 size = info.guest_size;
    if (size == 0 || out.Size() < out_offset + size) {
        return false;
    }
    if (!info.props.is_tiled) {
        auto copies = MipCopies(info);
        for (auto& copy : copies) {
            copy.buffer_offset += out_offset;
        }
        list.CopyImageToBuffer(entry.image.get(), &out, copies);
        return true;
    }
    const Rhi::Pipeline* tile = TilingPipeline(info, true);
    // PERF-314: GT Sport writes the same small pictures back thousands of times a second; making
    // three buffers for each write-back was most of the recorder's time in write-backs. The
    // picture's buffers are reused: its size and tiling never change, and every write-back is
    // recorded on the one queue, ordered after the last by the barriers below. Freed with the
    // entry (after the GPU is done with them, as before).
    if (!entry.download_params) {
        entry.download_params = TilingParamsBuffer(info);
    }
    if (!entry.download_linear || entry.download_linear->Size() < size) {
        entry.download_linear = device.CreateBuffer({.size = size,
                                                     .usage = Rhi::BufferUsageStorage |
                                                              Rhi::BufferUsageTransferDst,
                                                     .memory = Rhi::MemoryKind::DeviceLocal});
    }
    if (!entry.download_tiled || entry.download_tiled->Size() < size) {
        entry.download_tiled = device.CreateBuffer({.size = size,
                                                    .usage = Rhi::BufferUsageStorage |
                                                             Rhi::BufferUsageTransferSrc,
                                                    .memory = Rhi::MemoryKind::DeviceLocal});
    }
    Rhi::Buffer* params_buffer = entry.download_params.get();
    Rhi::Buffer* linear = entry.download_linear.get();
    Rhi::Buffer* tiled = entry.download_tiled.get();
    if (!tile || !params_buffer || !linear || !tiled) {
        return false;
    }
    list.CopyImageToBuffer(entry.image.get(), linear, MipCopies(info));
    list.Barrier(Rhi::ScopeTransfer, Rhi::ScopeCompute);
    list.BindPipeline(tile);
    const std::array<Rhi::Binding, 3> bindings{{
        {.slot = 0, .type = Rhi::BindingType::StorageBuffer, .buffer = tiled, .offset = 0,
         .size = size},
        {.slot = 1, .type = Rhi::BindingType::StorageBuffer, .buffer = linear, .size = size},
        {.slot = 2, .type = Rhi::BindingType::UniformBuffer, .buffer = params_buffer,
         .size = sizeof(TilingParams)},
    }};
    list.PushBindings(bindings);
    list.Dispatch(static_cast<u32>((size / (info.num_bits / 8)) / 64), 1, 1);
    list.Barrier(Rhi::ScopeCompute, Rhi::ScopeTransfer);
    list.CopyBuffer(tiled, 0, &out, out_offset, size);
    // Also orders this write-back's reads of the scratch buffers before the next one's writes.
    list.Barrier(Rhi::ScopeTransfer, Rhi::ScopeCompute | Rhi::ScopeHost | Rhi::ScopeTransfer);
    return true;
}

} // namespace Native
