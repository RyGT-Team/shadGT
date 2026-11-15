#include <cstdio>
namespace Vulkan {
bool RytyWideScene(); // WIDE-001 (ryty_game_patch.cpp)
}
#include "ryty_gpu/native/unique_upload_batch.h"
#include "ryty_gpu/native/upload_cause.h"
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <condition_variable>
#include <deque>
#include <stop_token>
#include <thread>
#include <algorithm>
#include <bit>
#include <cstring>

#include "common/diagnostics.h"
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
#include "video_core/texture_cache/packed_float.h"
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <thread>
#include <xxhash.h>
#include "common/thread.h"

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
    std::array<u32, VideoCore::MAX_MIPS> valid_width;
    std::array<u32, VideoCore::MAX_MIPS> valid_height;
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
        const char* env = Common::DiagEnv("SHADGT_NATIVE_WATCH_TEXTURE");
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
    // DIAG-167 (SHADGT_NATIVE_WATCH_RANGE=<hex lo>-<hex hi>): images starting in that range too
    // (pictures whose address changes from run to run).
    static const std::pair<VAddr, VAddr> range = [] {
        const char* env = Common::DiagEnv("SHADGT_NATIVE_WATCH_RANGE");
        if (!env || !*env) {
            return std::pair<VAddr, VAddr>{};
        }
        char* end_at = nullptr;
        const VAddr lo = std::strtoull(env, &end_at, 16);
        const VAddr hi = end_at && *end_at == '-' ? std::strtoull(end_at + 1, nullptr, 16) : lo + 1;
        return std::pair<VAddr, VAddr>{lo, hi};
    }();
    return range.first < range.second && begin >= range.first && begin < range.second;
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
        a.props.is_block != b.props.is_block || !a.PackedOrdersCompatible(b) || a.type != b.type) {
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
            // SHADGT_NATIVE_SHARED_TARGETS=0 turns it off (diagnosis).
            static const bool shared_targets = [] {
                const char* env = std::getenv("SHADGT_NATIVE_SHARED_TARGETS");
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
                    c.pixel_format == info.pixel_format &&
                    c.PackedOrdersCompatible(info) && c.resources.levels == 1 &&
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
                    // The old smaller target will no longer receive the draws.
                    // Make later texture lookups resolve to the larger picture.
                    if (exact) MarkReused(*exact);
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
        if (!render_target && (info.props.is_depth || Vulkan::LiverpoolToVK::IsFormatDepthCompatible(
                                                          info.pixel_format))) {
            // NAT-040: a depth buffer read as a texture (the T# names R32/R16, the image is
            // D32/D16): the depth image itself, viewed through its depth aspect. NAT-121: before a
            // texture of the same layout (an upload of the memory under the depth buffer, made
            // before it existed, would hide it: GT Sport's shadow cascades, a 256x256 depth
            // target with 14 layers read with 13). NAT-126: only for depth-like formats (a
            // B10G11R11 bloom picture in the scratch pool matched a bigger depth buffer there).
            // FIX-206: also a T# the shader samples as depth (a compare sampler: the format is
            // promoted to D32/D16): GT7's shadow cascades are a 256x256 D32S8 target with 12
            // layers, read as D32 with 2 to 11; they were uploaded from memory, which never holds
            // what the GPU drew, and nothing had a shadow. Of the depth targets there (GT7 also
            // draws a 5-layer one at that address), the one drawn last.
            static const bool depth_textures = [] {
                const char* env = std::getenv("SHADGT_NATIVE_DEPTH_TEXTURES"); // FIX-206 off: =0
                return !(env && env[0] == '0');
            }();
            ImageEntry* depth = nullptr;
            for (auto it = first; it != last; ++it) {
                const auto& cached = it->second->info;
                if (cached.props.is_depth && cached.num_bits == info.num_bits &&
                    (depth_textures || !info.props.is_depth) &&
                    !it->second->reused.load(std::memory_order_relaxed) &&
                    cached.resources.layers >= info.resources.layers &&
                    CompatibleLayerStrides(cached, info) &&
                    ((cached.size.width == info.size.width &&
                      cached.size.height == info.size.height) ||
                     (cached.pitch == info.pitch && cached.size.width >= info.size.width &&
                      cached.size.height >= info.size.height)) && // NAT-113
                    (!depth || it->second->write_version.load(std::memory_order_relaxed) >
                                   depth->write_version.load(std::memory_order_relaxed))) {
                    depth = it->second.get();
                }
            }
            if (depth) {
#ifndef NDEBUG
                ++stats.hits;
#endif
                depth->last_use.store(frame, std::memory_order_relaxed);
                return depth;
            }
        }
        if (!render_target && !info.props.is_depth) {
            // FIX-219: a colour texture that reads the first layers of a layered target of its own
            // layout (size, mips, format, pitch, tiling): the target itself, viewed through those
            // layers, as NAT-040 does for depth. GT7 draws every object's reflection probe into a
            // 64x64 B10G11R11 target of 7 mips and 2048 layers, and the car shaders read it as
            // 1464 layers: no target had that layout, the texture was uploaded from memory, which
            // never holds what the GPU drew, and parts of the cars reflected black (blocks on
            // the car in the race start's close-ups). SHADGT_NATIVE_LAYER_VIEWS=1: on.
            static const bool layer_views = [] {
                const char* env = std::getenv("SHADGT_NATIVE_LAYER_VIEWS");
                return env && env[0] == '1'; // off unless asked: it did not fix what it was made for
            }();
            ImageEntry* layered = nullptr;
            for (auto it = first; layer_views && it != last; ++it) {
                const auto& c = it->second->info;
                if (!it->second->from_cpu && it->second->image &&
                    !it->second->reused.load(std::memory_order_relaxed) && !c.props.is_depth &&
                    c.resources.layers > info.resources.layers &&
                    c.resources.levels == info.resources.levels &&
                    c.size.width == info.size.width && c.size.height == info.size.height &&
                    c.size.depth == info.size.depth && c.pitch == info.pitch &&
                    c.tile_mode == info.tile_mode && c.num_bits == info.num_bits &&
                    c.num_samples == info.num_samples && c.pixel_format == info.pixel_format &&
                    c.PackedOrdersCompatible(info) &&
                    (!layered || it->second->write_version.load(std::memory_order_relaxed) >
                                     layered->write_version.load(std::memory_order_relaxed))) {
                    layered = it->second.get();
                }
            }
            if (layered) {
#ifndef NDEBUG
                ++stats.hits;
#endif
                layered->last_use.store(frame, std::memory_order_relaxed);
                return layered;
            }
        }
#ifndef NDEBUG
        if (!render_target && WatchedTexture(info.guest_address, info.guest_address + 1) &&
            WatchSample()) {
            // DIAG-221 (diagnosis, with SHADGT_NATIVE_WATCH_TEXTURE): a texture lookup that found no
            // target of its layout: the pictures at its address and how their layouts differ.
            std::string others;
            for (auto it = first; it != last; ++it) {
                const auto& c = it->second->info;
                others += fmt::format(" | {} {}x{} pitch {} tile {} bits {} fmt {} lv {} ly {}{}{}",
                                      it->second->from_cpu ? "texture" : "target", c.size.width,
                                      c.size.height, c.pitch, u32(c.tile_mode), c.num_bits,
                                      u32(c.pixel_format), c.resources.levels, c.resources.layers,
                                      it->second->reused.load() ? " reused" : "",
                                      SameLayout(c, info) ? " same-layout" : "");
            }
            LOG_WARNING(Render_Vulkan,
                        "DIAG-221: texture {:#x} {}x{} pitch {} tile {} bits {} fmt {}: no target; "
                        "at the address:{}",
                        info.guest_address, info.size.width, info.size.height, info.pitch,
                        u32(info.tile_mode), info.num_bits, u32(info.pixel_format), others);
        }
#endif
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
                    c.PackedOrdersCompatible(info) &&
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
        // FIX-232: logged the first 8 times and then every 1000th (a tester's log grew to
        // 484 MB in 22 minutes of one format the driver refused).
        static std::atomic<u32> missing{};
        if (const u32 n = ++missing; n <= 8 || n % 1000 == 0) {
            LOG_WARNING(Render_Vulkan, "NAT-020: no image for {:#x} ({}x{}, format {}); {} so far",
                        info.guest_address, info.size.width, info.size.height,
                        vk::to_string(info.pixel_format), n);
        }
        return nullptr;
    }
    entry->last_use.store(frame, std::memory_order_relaxed);
    ImageEntry* result;
    bool remembered = false;
    // A CPU invalidation must see either the journal entry or the fully indexed target.
    std::unique_lock fill_lock{recent_fills_mutex};
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
        static const bool remember_fills = [] {
            const char* env = std::getenv("SHADGT_NATIVE_FILL_NEW_TARGETS");
            return !(env && env[0] == '0');
        }();
        if (render_target && info.num_samples == 1 && remember_fills) {
            if (auto fill = recent_fills.Find(info.guest_address, info.guest_size, frame)) {
                result->rewrite = ImageEntry::Rewrite::Clear;
                result->rewrite_pattern = fill->pattern;
                result->rewrite_bytes = info.guest_size;
                result->rewrite_clock = fill->clock;
                result->remembered_fill_pending = true;
                result->remembered_fill_receipt =
                    std::make_shared<RememberedFillReceipt>(remembered_fill_count);
                remembered = true;
            }
        }
        shard.images.emplace(info.guest_address, std::move(entry));
        IndexEntry(*result);
        const std::pair<VAddr, VAddr> range{info.guest_address,
                                            info.guest_address + std::max<u64>(info.guest_size, 1)};
        NoteChanges({&range, 1}, 1); // OPT-002, OPT-010
    }
    fill_lock.unlock();
#ifndef NDEBUG
    if (!render_target && Vulkan::LiverpoolToVK::IsFormatDepthCompatible(info.pixel_format)) {
        // DIAG-199 (diagnosis): a depth-like texture (R32/R16) made over a depth target's memory
        // without being that target (NAT-040 reads only one at its address and size): its
        // picture comes from memory, which never holds what the GPU drew (shadow maps).
        static std::atomic<u32> logged{};
        ForEachOverlapping(info.guest_address,
                           info.guest_address + std::max<u64>(info.guest_size, 1),
                           [&](ImageEntry& other) {
                               if (other.from_cpu || !other.info.props.is_depth ||
                                   logged.load(std::memory_order_relaxed) >= 40) {
                                   return;
                               }
                               ++logged;
                               LOG_WARNING(Render_Vulkan,
                                           "DIAG-199: texture {:#x}+{:#x} {}x{} pitch {} {} "
                                           "layers {} mips {} tile {} over depth target "
                                           "{:#x}+{:#x} {}x{} pitch {} {} layers {} tile {}",
                                           info.guest_address, info.guest_size, info.size.width,
                                           info.size.height, info.pitch,
                                           vk::to_string(info.pixel_format),
                                           info.resources.layers, info.resources.levels,
                                           u32(info.tile_mode), other.info.guest_address,
                                           other.info.guest_size, other.info.size.width,
                                           other.info.size.height, other.info.pitch,
                                           vk::to_string(other.info.pixel_format),
                                           other.info.resources.layers,
                                           u32(other.info.tile_mode));
                           });
    }
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
        // (diagnosis) SHADGT_NATIVE_ZERO_NEW_TARGETS=1: a new target starts as zeros instead.
        // =<hex lo>-<hex hi>: only targets starting in that range.
        static const std::pair<VAddr, VAddr> zero_range = [] {
            const char* env = std::getenv("SHADGT_NATIVE_ZERO_NEW_TARGETS");
            if (!env || !*env) {
                return std::pair<VAddr, VAddr>{};
            }
            if (env[0] == '1' && env[1] == '\0') {
                return std::pair<VAddr, VAddr>{0, ~VAddr{0}};
            }
            char* end = nullptr;
            const VAddr lo = std::strtoull(env, &end, 16);
            const VAddr hi = end && *end == '-' ? std::strtoull(end + 1, nullptr, 16) : lo + 1;
            return std::pair<VAddr, VAddr>{lo, hi};
        }();
        const bool zero_new = info.guest_address >= zero_range.first &&
                              info.guest_address < zero_range.second;
        // FIX-GT7-036: a target made just after a fill that covers it starts as that fill
        // (remembered above, under recent_fills_mutex).
        if (!remembered && zero_new && info.num_samples == 1) {
            result->rewrite = ImageEntry::Rewrite::Clear;
            result->rewrite_pattern = {0, 0};
            result->rewrite_bytes = info.guest_size;
        } else if (!remembered && info.num_samples == 1) {
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

void ImageRegistry::NoteChanges(std::span<const std::pair<VAddr, VAddr>> ranges, u8 kind) {
    std::scoped_lock lock{changes_mutex};
    const u64 next = generation.load(std::memory_order_relaxed) + 1;
    for (const auto& [begin, end] : ranges) {
        changes[change_count++ % changes.size()] = {next, begin, end, kind};
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
                untouched_no[change.kind >= 1 && change.kind <= 3 ? change.kind : 1].fetch_add(
                    1, std::memory_order_relaxed); // DIAG-GTS-401
                untouched_no_begin.store(change.begin, std::memory_order_relaxed); // DIAG-GTS-402
                return false;
            }
        }
    }
    // Every change kept is newer: the log reaches back far enough only if none was lost.
    if (change_count > changes.size()) {
        untouched_no[0].fetch_add(1, std::memory_order_relaxed); // DIAG-GTS-401
    }
    return change_count <= changes.size();
}

bool ImageRegistry::UntouchedHolding(u64 since, std::span<const std::pair<VAddr, VAddr>> ranges,
                                     std::span<const std::pair<VAddr, VAddr>> held) {
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
        if (change.kind == 2) {
            // An evicted picture is gone for whoever held it, and for nobody else: a lookup
            // that answered with another picture still answers with that one.
            for (const auto& [begin, end] : held) {
                if (change.begin == begin && change.end == end) {
                    untouched_no[2].fetch_add(1, std::memory_order_relaxed);
                    untouched_no_begin.store(change.begin, std::memory_order_relaxed);
                    return false;
                }
            }
            continue;
        }
        for (const auto& [begin, end] : ranges) {
            if (change.begin < end && change.end > begin) {
                untouched_no[change.kind == 3 ? 3 : 1].fetch_add(1, std::memory_order_relaxed);
                untouched_no_begin.store(change.begin, std::memory_order_relaxed);
                return false;
            }
        }
    }
    if (change_count > changes.size()) {
        untouched_no[0].fetch_add(1, std::memory_order_relaxed);
    }
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
    // FIX-GT7-127: a level of a block-compressed texture written by the GPU as one uncompressed
    // texel per block. GT7's Scapes compress their HDR photo (6024x4016) to BC6H in a compute
    // shader that binds each level as an R32G32B32A32Uint storage image over the level's padded
    // area (2048x1024 for level 0, at the texture's address; 1024x512 for level 1, at its offset),
    // and the scene samples the BC6H texture. Without a place the texture read guest memory,
    // which only the GPU had written: zeros, a black background in the editor and in every photo.
    // The copy then counts in source texels, one per block (Vulkan's rule between compressed and
    // uncompressed formats). SHADGT_NATIVE_BLOCK_TARGETS=0: as before.
    if (texture.props.is_block && !target.props.is_block) {
        static const bool block_targets = [] {
            const char* env = std::getenv("SHADGT_NATIVE_BLOCK_TARGETS");
            return !(env && env[0] == '0');
        }();
        const s32 mip = block_targets ? texture.BlockLevelOf(target) : -1;
        if (mip < 0) {
            return {};
        }
        SourcePlace place{mip, 0, (std::max(texture.size.width >> mip, 1u) + 3) / 4,
                          (std::max(texture.size.height >> mip, 1u) + 3) / 4};
        place.blocks = true;
#ifndef NDEBUG
        static std::atomic<u32> logged{};
        if (logged++ < 16) {
            LOG_INFO(Render_Vulkan,
                     "FIX-GT7-127: texture {:#x} {}x{} {} level {} made of target {:#x} {}x{} {} "
                     "({}x{} blocks)",
                     texture.guest_address, texture.size.width, texture.size.height,
                     vk::to_string(texture.pixel_format), mip, target.guest_address,
                     target.size.width, target.size.height, vk::to_string(target.pixel_format),
                     place.width, place.height);
        }
#endif
        return place;
    }
    if (target.props.is_depth || texture.props.is_depth || target.props.is_block ||
        texture.props.is_block || !target.PackedOrdersCompatible(texture) ||
        target.num_bits != texture.num_bits ||
        target.num_samples != texture.num_samples) {
        return {};
    }
    // A crop: the texture is the corner of the target in the same layout (as IsSubrectOf, any
    // colour format of the texel size). FIX-195: of every layer too, as NAT-113 draws a smaller
    // layered target into a larger one: GT7 writes its race sky cube (256x256, six layers, pitch
    // 512) with one dispatch per face, those writes go into the 512x512 cube made earlier, and the
    // sky shader's read of the 256x256 cube found nothing of its layout, read zeros and drew the
    // sky black.
    // FIX-GT7-058: of a target's first level too, as its other levels don't move that one. GT7's
    // championship course maps resolve into the scratch pool's target at 0x100a0b0000, and when
    // that was a 256x256 pyramid left by another pass, their 200x180 texture found no place and
    // read guest memory: whatever colours it held, then a black square, with the map in the target.
    // SHADGT_NATIVE_CROP_OF_MIPS=0: as before.
    static const bool crop_of_mips = [] {
        const char* env = std::getenv("SHADGT_NATIVE_CROP_OF_MIPS");
        return !(env && env[0] == '0');
    }();
    if (target.guest_address == texture.guest_address &&
        (target.resources.levels == 1 || crop_of_mips) && texture.resources.levels == 1 &&
        texture.resources.layers <= target.resources.layers && target.pitch == texture.pitch &&
        target.tile_mode == texture.tile_mode && target.array_mode == texture.array_mode &&
        ((target.micro_tiled_mips ^ texture.micro_tiled_mips) & 1u) == 0 &&
        target.bank_swizzle == texture.bank_swizzle &&
        target.alt_tile == texture.alt_tile &&
        target.mips_layout[0].pitch == texture.mips_layout[0].pitch &&
        target.mips_layout[0].offset == texture.mips_layout[0].offset &&
        texture.size.width <= target.size.width && texture.size.height <= target.size.height &&
        (texture.size.width < target.size.width || texture.size.height < target.size.height) &&
        target.size.depth == 1 && texture.size.depth == 1 &&
        target.type == AmdGpu::ImageType::Color2D && texture.type == target.type) {
#ifndef NDEBUG
        if (target.resources.levels != 1) {
            static std::atomic<u32> logged{};
            if (const u32 n = logged++; n < 8) {
                LOG_WARNING(Render_Vulkan,
                            "FIX-GT7-058: texture {:#x} {}x{} {} is a corner of target {}x{} {}, "
                            "{} levels",
                            texture.guest_address, texture.size.width, texture.size.height,
                            vk::to_string(texture.pixel_format), target.size.width,
                            target.size.height, vk::to_string(target.pixel_format),
                            target.resources.levels);
            }
        }
#endif
        return {0, 0, texture.size.width, texture.size.height};
    }
    // A mip or layer of the texture drawn on its own.
    // FIX-GT7-134: also a target a little larger than the mip, within its padded size. GT7's
    // depth of field (Scapes' editor and photos, background blurring) builds an 8-level pyramid
    // drawing each level as its own target sized to its extent (345x248, 172x124, ...) while the
    // texture that samples the pyramid is 690x493, 690x479 or 690x496 (levels 345x246, 172x123,
    // ...): no level was found, the blurred levels read guest memory (zeros) and the blurred
    // background came out black. The copy is the mip's size. SHADGT_NATIVE_PADDED_MIPS=0: as before.
    static const bool padded_mips = [] {
        const char* env = std::getenv("SHADGT_NATIVE_PADDED_MIPS");
        return !(env && env[0] == '0');
    }();
    if (const s32 mip = target.MipOf(texture, padded_mips); mip >= 0) {
        const s32 layer = target.SliceOf(texture, mip, padded_mips);
        if (layer < 0 || u32(layer) >= std::max<u32>(texture.resources.layers, 1)) {
            return {};
        }
        return {mip, layer, std::min(target.size.width, std::max(texture.size.width >> mip, 1u)),
                std::min(target.size.height, std::max(texture.size.height >> mip, 1u))};
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

SourcePlace PartOf(const VideoCore::ImageInfo& part, const VideoCore::ImageInfo& whole) {
    if (part.props.is_depth || whole.props.is_depth || part.props.is_block ||
        whole.props.is_block || part.num_bits != whole.num_bits ||
        !part.PackedOrdersCompatible(whole) ||
        part.num_samples != whole.num_samples || part.resources.levels != 1 ||
        (whole.resources.levels <= 1 && whole.resources.layers <= 1) ||
        part.size.depth != 1 || whole.size.depth != 1) {
        return {};
    }
    const s32 mip = part.MipOf(whole);
    if (mip < 0) {
        return {};
    }
    const s32 layer = part.SliceOf(whole, mip);
    if (layer < 0 || u32(layer) >= std::max<u32>(whole.resources.layers, 1)) {
        return {};
    }
    return {mip, layer, part.size.width, part.size.height};
}

bool ImageRegistry::SmallRewritesOn() {
    static const bool on = [] {
        const char* env = std::getenv("SHADGT_NATIVE_SMALL_REWRITES");
        return !(env && env[0] == '0');
    }();
    return on;
}

bool ImageRegistry::RewrittenSmall(ImageEntry& entry) {
    // FIX-202: a storage buffer much larger than a texture is often a scratch pool written in
    // part (NAT-129 ignores it: GT Sport's would upload every texture in it at each dispatch).
    // A small texture in it is uploaded again at its next use after such a write, from the
    // region the write went to: GT7 computes the driver's lighting into 4x4 RGBA16F textures
    // with one dispatch over a larger buffer, and when the texture was first used before that
    // dispatch it kept the garbage memory held then (Inf and NaN): the driver glowed in a random
    // colour for the whole session, half the launches. SHADGT_NATIVE_SMALL_REWRITES=0: ignored.
    const u64 rewritten = entry.gpu_rewritten.load(std::memory_order_relaxed);
    if (rewritten <= entry.rewrite_seen) {
        return false;
    }
    entry.rewrite_seen = rewritten;
    {
        std::scoped_lock lock{entry.sources_mutex};
        if (!entry.sources.empty()) {
            return false; // made of targets: their pictures come in instead (NAT-104)
        }
    }
    if (entry.info.num_samples != 1) {
        return false;
    }
    MarkStale(entry, Stale::GpuWrite);
    return true;
}

int ImageRegistry::LayersDifferingFromUpload(const ImageEntry& entry) const {
    // FIX-GT7-027 (diagnosis): the upload's per-layer hashes live in entry.layer_hash
    // (LayerHashState), which does not expose them: not known here.
    (void)entry;
    return -1;
}

bool TargetPartsOn() {
    static const bool on = [] {
        const char* env = std::getenv("SHADGT_NATIVE_TARGET_PARTS");
        return !(env && env[0] == '0');
    }();
    return on;
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
            // FIX-197: targets made of targets. GT7 lights its reflection probes in a 64x64
            // cube target (seven mips), then writes mips 1-6 of each face as targets of their
            // own at those mips' addresses (one 2D image per face and mip), and its convolution
            // samples the cube at mips 1-6: they were never written in the cube's image, the
            // probes' 32x32 lighting came out zero, and the pit crew and the crowd were drawn
            // as black silhouettes. A target that is a mip or layers of another is its source
            // from then on, as for textures (NAT-104/111): copied in before the other's next read.
            if (&entry == &target || entry.reused.load(std::memory_order_relaxed) ||
                !entry.image || !TargetPartsOn()) {
                return;
            }
            ImageEntry* whole = &entry;
            ImageEntry* part = &target;
            auto place = PartOf(part->info, whole->info);
            if (place.mip < 0) {
                std::swap(whole, part);
                place = PartOf(part->info, whole->info);
            }
#ifndef NDEBUG
            if (WatchedTexture(begin, end)) {
                LOG_WARNING(Render_Vulkan, "FIX-197 watch: new target {:#x} {}x{} mips {} layers {} "
                            "and target {:#x} {}x{} mips {} layers {}: {}", begin,
                            target.info.size.width, target.info.size.height,
                            target.info.resources.levels, target.info.resources.layers,
                            entry.info.guest_address, entry.info.size.width,
                            entry.info.size.height, entry.info.resources.levels,
                            entry.info.resources.layers,
                            place.mip >= 0 ? fmt::format("part at mip {} layer {}", place.mip,
                                                         place.layer)
                                           : std::string{"no part"});
            }
#endif
            if (place.mip < 0) {
                return;
            }
            // (diagnosis) SHADGT_NATIVE_TARGET_PARTS=2: only layered wholes; =3: only 2D ones.
            static const char parts_mode = [] {
                const char* env = std::getenv("SHADGT_NATIVE_TARGET_PARTS");
                return env && *env ? env[0] : '1';
            }();
            // (diagnosis) SHADGT_NATIVE_TARGET_PARTS_ONLY=<hex address>: only that whole.
            static const VAddr parts_only = [] {
                const char* env = std::getenv("SHADGT_NATIVE_TARGET_PARTS_ONLY");
                return env && *env ? VAddr(std::strtoull(env, nullptr, 16)) : VAddr{0};
            }();
            if ((parts_mode == '2' && whole->info.resources.layers <= 1) ||
                (parts_mode == '3' && whole->info.resources.layers > 1) ||
                (parts_only != 0 && whole->info.guest_address != parts_only)) {
                return;
            }
            {
                std::scoped_lock sources_lock{whole->sources_mutex};
                whole->sources.push_back({part, place});
            }
            whole->sources_dirty.store(true, std::memory_order_relaxed);
            std::scoped_lock part_lock{part->sources_mutex};
            part->dependents.push_back(whole);
            if (++stats.target_parts <= 600) {
                LOG_INFO(Render_Vulkan, "FIX-197: target {:#x} {}x{} (fmt {}) is mip {} layer {} "
                         "of target {:#x} {}x{} mips {} layers {} (fmt {})",
                         part->info.guest_address, part->info.size.width,
                         part->info.size.height, vk::to_string(part->info.pixel_format),
                         place.mip, place.layer, whole->info.guest_address,
                         whole->info.size.width, whole->info.size.height,
                         whole->info.resources.levels, whole->info.resources.layers,
                         vk::to_string(whole->info.pixel_format));
            }
            return;
        }
        const auto place = PlaceOf(target.info, entry.info);
#ifndef NDEBUG
        static const bool small_links = Common::DiagEnv("SHADGT_DIAG_SMALL_LINKS") != nullptr;
        static std::atomic<u32> small_logged{};
        if (small_links && place.mip >= 0 && entry.info.size.width <= 8 &&
            entry.info.size.height <= 8 && small_logged++ < 200) {
            LOG_WARNING(Render_Vulkan, "DIAG-181: new target {:#x} {}x{} {} mips {} layers {} "
                        "linked to texture {:#x} {}x{} {} at mip {} layer {} src mip {} layer {}",
                        begin, target.info.size.width, target.info.size.height,
                        vk::to_string(target.info.pixel_format), target.info.resources.levels,
                        target.info.resources.layers, entry.info.guest_address,
                        entry.info.size.width, entry.info.size.height,
                        vk::to_string(entry.info.pixel_format), place.mip, place.layer,
                        place.src_mip, place.src_layer);
        }
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
#ifndef NDEBUG
                // DIAG-181 (diagnosis, SHADGT_DIAG_SMALL_LINKS=1): links of tiny textures.
                static const bool small_links = Common::DiagEnv("SHADGT_DIAG_SMALL_LINKS") != nullptr;
                static std::atomic<u32> small_logged{};
                if (small_links && texture.info.size.width <= 8 && texture.info.size.height <= 8 &&
                    small_logged++ < 200) {
                    LOG_WARNING(Render_Vulkan, "DIAG-181: new texture {:#x} {}x{} {} linked to "
                                "target {:#x} {}x{} {} mips {} layers {} at mip {} layer {} src "
                                "mip {} layer {}", begin, texture.info.size.width,
                                texture.info.size.height, vk::to_string(texture.info.pixel_format),
                                entry->info.guest_address, entry->info.size.width,
                                entry->info.size.height, vk::to_string(entry->info.pixel_format),
                                entry->info.resources.levels, entry->info.resources.layers,
                                place.mip, place.layer, place.src_mip, place.src_layer);
                }
#endif
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

void ImageRegistry::ForEachImage(const std::function<void(ImageEntry&)>& fn) {
    for (Shard& shard : shards) {
        std::shared_lock lock{shard.mutex};
        for (const auto& [base, entry] : shard.images) {
            // Match ForEachOverlapping(0, ~VAddr{0}), including overflow edges.
            const u64 span = std::clamp<u64>(entry->info.guest_size, 1, MaxIndexedSize);
            if (base < ~VAddr{0} && base + span > base) fn(*entry);
        }
    }
}

void ImageRegistry::ForEachImageInShard(u32 index, const std::function<void(ImageEntry&)>& fn) {
    Shard& shard = shards[index % NumShards];
    std::shared_lock lock{shard.mutex};
    for (const auto& [base, entry] : shard.images) {
        const u64 span = std::clamp<u64>(entry->info.guest_size, 1, MaxIndexedSize);
        if (base < ~VAddr{0} && base + span > base) fn(*entry);
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
    // PERF-GTS-385 (SHADGT_EVICT_SCAN_SHARDS=<1..64>, default 64 = as before): the bounded
    // per-frame scan read every picture's last use, a cache miss each: 7.5% of the recorder's
    // CPU time in a 20-car race at Tokyo (about 1.2 ms a frame), and a second walk of every
    // shard whenever something went (1.8%). With fewer shards a frame the scan goes round
    // the table over several frames; a frame's budget is still filled whenever the shards
    // it visits hold that many idle pictures.
    static const u32 scan_shards = [] {
        const char* env = std::getenv("SHADGT_EVICT_SCAN_SHARDS");
        // PERF-GTS-407: 8 shards a frame unless set.
        const u32 value = env && *env ? u32(std::strtoul(env, nullptr, 10)) : 8;
        return std::clamp<u32>(value, 1, NumShards);
    }();
    const u32 visits = bounded ? scan_shards : NumShards;
    if (bounded) eviction_shard_cursor = (first + visits) % NumShards;
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
    // Pending receipts without a submitted stream retire with their entry. Submitted
    // receipts remain owned by the scheduler until the graphics tick completes.
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
        NoteChanges(ranges, 2); // OPT-002, OPT-010
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
    entry.last_stale = static_cast<u8>(why); // DIAG-197 (diagnosis)
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
    // CPU-texture invalidations of every size wait for actual image use. In particular,
    // OnUnmap runs before guest memory is released: eagerly queuing here re-imports
    // and uploads a discarded texture while the unmapping thread waits for us.
    // Preserve dirty/force flags so a later use after remapping still refreshes it.
    // Small discarded font images can share pages with a subsequently created
    // fiber stack. An unrelated flush must not refresh them and re-arm a write
    // watch there. Actual bindings (including memo hits) admit their dirty image.
    // Explicit RequestUpload separately admits its required rewrite immediately.
    const bool defer = entry.from_cpu;
    if (!already_dirty && !defer) {
        std::scoped_lock upload_lock{upload_mutex};
        pending_uploads.push_back(&entry);
    }
}

void ImageRegistry::ForgetFillHistory(VAddr address, u64 size, const ImageEntry* authoritative) {
    std::scoped_lock lock{recent_fills_mutex};
    recent_fills.Forget(address, size);
    if (!authoritative || remembered_fill_count->load(std::memory_order_relaxed) == 0 ||
        size == 0 || size > ~u64{0} - address) return;
    // A different target cannot apply an older remembered clear after this
    // image write. Retire it from future lookups; its in-flight image stays alive.
    ForEachOverlapping(address, address + size, [&](ImageEntry& entry) {
        if (&entry != authoritative && !entry.from_cpu && entry.remembered_fill_receipt &&
            entry.remembered_fill_receipt->live.load(std::memory_order_acquire)) {
            MarkReused(entry);
        }
    });
}

void ImageRegistry::ForgetFills(VAddr address, u64 size) {
    std::scoped_lock lock{recent_fills_mutex};
    recent_fills.Forget(address, size);
    if (remembered_fill_count->load(std::memory_order_relaxed) == 0 || size == 0 ||
        size > ~u64{0} - address) return;
    ForEachOverlapping(address, address + size, [&](ImageEntry& entry) {
        if (entry.remembered_fill_receipt &&
            entry.remembered_fill_receipt->live.load(std::memory_order_acquire)) {
            entry.remembered_fill_invalid = true;
            MarkStale(entry, Stale::GpuWrite);
        }
    });
}

bool ImageRegistry::FilledWhole(VAddr address, u64 size) {
    const VAddr end = address + std::max<u64>(size, 1);
    std::scoped_lock lock{recent_fills_mutex};
    return std::ranges::any_of(recent_fills.Ranges(), [&](const std::pair<VAddr, VAddr>& fill) {
        return fill.first <= address && end <= fill.second;
    });
}

std::vector<std::pair<VAddr, VAddr>> ImageRegistry::RecentFillRanges() {
    std::scoped_lock lock{recent_fills_mutex};
    return recent_fills.Ranges();
}

void ImageRegistry::ExpireRecentFills(u64 frame) {
    std::scoped_lock lock{recent_fills_mutex};
    recent_fills.Expire(frame);
}

ImageRegistry::FillClaim ImageRegistry::ClaimRememberedFill(ImageEntry& entry) {
    std::scoped_lock lock{recent_fills_mutex};
    if (!entry.remembered_fill_pending) return {};
    entry.remembered_fill_pending = false;
    return {!entry.remembered_fill_invalid, entry.remembered_fill_receipt};
}

void ImageRegistry::Invalidate(VAddr address, u64 size, Stale why) {
    std::scoped_lock fill_lock{recent_fills_mutex};
    recent_fills.Forget(address, size);
    ForEachOverlapping(address, address + size, [&](ImageEntry& entry) {
        if (entry.from_cpu) {
            MarkStale(entry, why);
        } else {
            entry.guest_modified.store(true, std::memory_order_seq_cst);
            if (entry.remembered_fill_receipt &&
                entry.remembered_fill_receipt->live.load(std::memory_order_acquire)) {
                entry.remembered_fill_invalid = true;
                MarkStale(entry, why);
            }
            // Unmapping ends the allocation's lifetime, even before it was kept.
            // Retire it from lookup without destroying resources still in flight.
            if (why == Stale::Unmap || entry.kept.load(std::memory_order_seq_cst)) {
                MarkReused(entry); // FIX-157
            }
        }
    });
}

u32 ImageRegistry::InvalidateNewMapping(VAddr address, u64 size) {
    ForgetFills(address, size); // FIX-GT7-036
    u32 count = 0;
    ForEachOverlapping(address, address + std::max<u64>(size, 1), [&](ImageEntry& entry) {
        if (entry.from_cpu) {
            MarkStale(entry, Stale::Unmap); // FIX-GT7-077: forced, not skipped by NAT-109's hash
            ++count;
        }
    });
    return count;
}

void ImageRegistry::MarkReused(ImageEntry& entry) {
    if (entry.reused.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    // What Find returned for the range may change (OPT-002 memos look again).
    const std::pair<VAddr, VAddr> range{entry.info.guest_address,
                                        entry.info.guest_address +
                                            std::max<u64>(entry.info.guest_size, 1)};
    NoteChanges({&range, 1}, 3);
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

static bool LayerSamplesEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADGT_NATIVE_LAYER_SAMPLES");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

static bool LayerWholeHashEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADGT_NATIVE_LAYER_HASH");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

namespace {
// PERF-GTS-452: one thread that runs the layer checks, so the recorder does not hash them.
class LayerHashWorker {
public:
    static LayerHashWorker& Get() {
        static LayerHashWorker worker;
        return worker;
    }
    bool Post(std::function<void()> job) {
        {
            std::scoped_lock lock{mutex};
            if (queue.size() >= 256) return false;
            queue.push_back(std::move(job));
        }
        ready.notify_one();
        return true;
    }

private:
    LayerHashWorker() {
        std::thread([this] {
            Common::SetCurrentThreadName("shadGT:LayerHash");
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock lock{mutex};
                    ready.wait(lock, [&] { return !queue.empty(); });
                    job = std::move(queue.front());
                    queue.pop_front();
                }
                job();
            }
        }).detach();
    }
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> queue;
};

bool LayerHashAsyncEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADGT_NATIVE_LAYER_HASH_ASYNC");
        return !(env && env[0] == '0');
    }();
    return enabled;
}
} // namespace

bool ImageRegistry::LayersRewritten(ImageEntry& entry, u64 frame) {
    const bool samples = LayerSamplesEnabled();
    const bool whole = LayerWholeHashEnabled();
    const u64 size = entry.info.guest_size;
    const u32 layers = entry.info.resources.layers;
    auto& shared = *entry.layer_hash;
    if (!samples || !entry.from_cpu || layers < 16 || size <= (4ull << 20) ||
        size % layers != 0 || entry.gpu_backing_written.load(std::memory_order_acquire) ||
        !memory_clamp) return false;
    // PERF-GTS-452: a change the background check found since the last look.
    if (shared.changed.exchange(false, std::memory_order_acq_rel)) {
        MarkStale(entry, Stale::CpuWrite);
        return true;
    }
    if (shared.checked_frame.load(std::memory_order_relaxed) == frame ||
        memory_clamp(entry.info.guest_address, size) != size) return false;
    const u64 layer_size = size / layers;
    const VAddr address = entry.info.guest_address;
    const auto hash = [address, layer_size](u32 layer, u64 offset, u64 bytes) {
        return Core::HashGuestBytes(address + layer * layer_size + offset, bytes);
    };
    if (!LayerHashAsyncEnabled()) {
        shared.checked_frame.store(frame, std::memory_order_relaxed);
        std::scoped_lock lock{shared.mutex};
        const bool changed = shared.state.Changed(layers, layer_size, frame, hash, samples, whole);
        if (changed) MarkStale(entry, Stale::CpuWrite);
        return changed;
    }
    // At most one check in flight per texture; the recorder only posts it. Found changes are
    // applied at the texture's next use (a frame later, as the rotating whole-layer hashes
    // already take several frames to cover an array).
    if (shared.busy.exchange(true, std::memory_order_acq_rel)) return false;
    shared.checked_frame.store(frame, std::memory_order_relaxed);
    const u64 generation = shared.generation.load(std::memory_order_acquire);
    std::shared_ptr<LayerHashShared> keep = entry.layer_hash;
    const bool posted = LayerHashWorker::Get().Post([keep, layers, layer_size, frame, samples,
                                                     whole, generation, hash] {
        {
            std::scoped_lock lock{keep->mutex};
            const bool changed = keep->state.Changed(layers, layer_size, frame, hash, samples, whole);
            if (changed && keep->generation.load(std::memory_order_acquire) == generation) {
                keep->changed.store(true, std::memory_order_release);
            }
        }
        keep->busy.store(false, std::memory_order_release);
    });
    if (!posted) shared.busy.store(false, std::memory_order_release);
    return false;
}

void ImageRegistry::GpuWrote(VAddr address, u64 size, const std::array<u32, 2>* pattern,
                             bool exact, u64 clock, u64 frame, bool guest_authoritative) {
    {
        std::scoped_lock lock{recent_fills_mutex};
        recent_fills.Forget(address, size);
        if (pattern && guest_authoritative) {
            recent_fills.Record(address, size, *pattern, frame, clock);
        }
    }
#ifndef NDEBUG
    if (WatchedTexture(address, address + std::max<u64>(size, 1)) && WatchSample()) {
        static const bool dump_on_fill = Common::DiagEnv("SHADGT_FRAME_DUMP_ON_FILL") != nullptr;
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
                // FIX-202: a small texture is uploaded again at its next use instead (see
                // ImageRegistry::RewrittenSmall).
                if (entry.info.guest_size <= SmallRewriteSize && SmallRewritesOn()) {
                    u64 seen = entry.gpu_rewritten.load(std::memory_order_relaxed);
                    while (seen < clock && !entry.gpu_rewritten.compare_exchange_weak(
                                               seen, clock, std::memory_order_relaxed)) {
                    }
                }
#ifndef NDEBUG
                // DIAG-183 (diagnosis, SHADGT_DIAG_IGNORED_WRITES=1): such writes over tiny textures;
                // =<w>x<h>: over textures that size (GT7's 30x17 light grid, 136 KB).
                static const std::array<u32, 3> diag = [] {
                    std::array<u32, 3> d{};
                    if (const char* env = Common::DiagEnv("SHADGT_DIAG_IGNORED_WRITES")) {
                        d[0] = 1;
                        if (std::sscanf(env, "%ux%u", &d[1], &d[2]) != 2) {
                            d[1] = d[2] = 0;
                        }
                    }
                    return d;
                }();
                static std::atomic<u32> logged{};
                const bool sized = diag[1] != 0 && entry.info.size.width == diag[1] &&
                                   entry.info.size.height == diag[2];
                if (diag[0] && (sized || (diag[1] == 0 && entry.info.guest_size <= 4096)) &&
                    logged++ < 3000) {
                    LOG_WARNING(Render_Vulkan, "DIAG-183: storage write {:#x}+{:#x} over texture "
                                "{:#x} {}x{} {} ignored (clock {}, last use {})", address, size,
                                entry.info.guest_address, entry.info.size.width,
                                entry.info.size.height, vk::to_string(entry.info.pixel_format),
                                clock, entry.last_use.load(std::memory_order_relaxed));
                }
#endif
                return;
            }
            // Retain ownership even after clears/source links/force flags are consumed.
            entry.gpu_backing_written.store(true, std::memory_order_release);
            // WIDE-006: a texture made of targets (a crop of the scene) takes its picture from
            // them; a fill over it clears the targets, not it. Clearing the crop too blanked the
            // car selection's car every other frame with the wide scene (a fill now covered the
            // crop, which takes its picture from the scene only when the scene was drawn again).
            // WIDE-061: only fills, and only with the wide scene. Skipping every GPU write (copies,
            // compute writes) left stale data in textures the 11-07 renderer links to targets:
            // grass grew all over the Nordschleife's track, also with the wide scene off.
            if (pattern && Vulkan::RytyWideScene()) {
                std::scoped_lock lock{entry.sources_mutex};
                if (!entry.sources.empty()) {
                    return;
                }
            }
            // NAT-133: a fill that covers the whole texture fills it on the GPU at its next use
            // (GT Sport clears its environment cube and motion targets with fills, and textures
            // over that memory were uploaded again 60 times a second).
            if (pattern && address <= entry.info.guest_address &&
                address + size >= entry.info.guest_address + entry.info.guest_size) {
                entry.rewrite = ImageEntry::Rewrite::Clear;
                entry.rewrite_pattern = *pattern;
                entry.rewrite_bytes = entry.info.guest_size;
                entry.rewrite_clock = clock;
                // FIX-GT7-059: as FIX-209: kept, the same bytes written back by the CPU were taken
                // for "unchanged" and never uploaded, and the texture kept the fill. GT7's main
                // menu reads its sky tables from memory a race's fill covers: they stayed zero,
                // and after a race the menu's ground and buildings were black until the game
                // closed, whatever the time of day. SHADGT_NATIVE_FILL_FORGETS_HASH=0: as before.
                static const bool forget_hash = [] {
                    const char* env = std::getenv("SHADGT_NATIVE_FILL_FORGETS_HASH");
                    return !(env && env[0] == '0');
                }();
                if (forget_hash) {
                    entry.content_hash = 0;
#ifndef NDEBUG
                    static std::atomic<u32> logged{};
                    if (const u32 n = logged++; n < 40 || n % 2000 == 0) {
                        LOG_INFO(Render_Vulkan,
                                 "FIX-GT7-059: texture {:#x} {}x{} filled by {:#x}+{:#x} with {:#x} "
                                 "({} so far)",
                                 entry.info.guest_address, entry.info.size.width,
                                 entry.info.size.height, address, size, (*pattern)[0], n + 1);
                    }
#endif
                }
                return;
            }
            bool made_of_targets;
            {
                std::scoped_lock lock{entry.sources_mutex};
                made_of_targets = !entry.sources.empty();
            }
            // FIX-200: a texture made of targets whose memory a GPU buffer write rewrote whole
            // holds what the write left, as the GPU's memory does: it is uploaded again, and the
            // targets' pictures from before the write are no longer copied in (newer ones still
            // are). GT7's compute shaders write pictures as buffers into the scratch pool other
            // passes drew into before (0x100a0b0000), and the texture read there kept showing an
            // older target's picture: the far scenery of the replay's depth-of-field shots was
            // black. SHADGT_NATIVE_REWRITE_MADE_OF_TARGETS=0: such writes are ignored, as before.
            static const bool rewrite_made_of_targets = [] {
                const char* env = std::getenv("SHADGT_NATIVE_REWRITE_MADE_OF_TARGETS");
                return !(env && env[0] == '0');
            }();
            const bool whole = address <= entry.info.guest_address &&
                               address + size >= entry.info.guest_address + entry.info.guest_size;
            if (made_of_targets && whole && rewrite_made_of_targets) {
                u64 seen = entry.foreign_clock.load(std::memory_order_relaxed);
                while (seen < clock && !entry.foreign_clock.compare_exchange_weak(
                                           seen, clock, std::memory_order_relaxed)) {
                }
                made_of_targets = false;
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
    std::scoped_lock fill_lock{recent_fills_mutex};
    recent_fills.Forget(address, size);
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
            if (entry.remembered_fill_receipt &&
                entry.remembered_fill_receipt->live.load(std::memory_order_acquire)) {
                entry.remembered_fill_invalid = true;
                entry.force_upload.store(true, std::memory_order_relaxed);
                MarkStale(entry, Stale::CpuWrite);
            }
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
        params.valid_width[mip] = std::max(info.size.width >> mip, 1u);
        params.valid_height[mip] = std::max(info.size.height >> mip, 1u);
        if (info.props.is_block) {
            mip_info.pitch = std::max<u16>((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max<u16>((mip_info.height + 3) / 4, 1U);
        }
    }
    std::memcpy(buffer->Mapped(), &params, sizeof(params));
    return buffer;
}

const Rhi::Pipeline* ImageRegistry::TilingPipeline(const VideoCore::ImageInfo& info, bool tiler) {
  const u64 key = VideoCore::NativePackedTilingKey(VideoCore::TilingPipelineKey(info, tiler),
                                                    info.props.is_bgr_packed_float,
                                                    info.props.is_tiled);
  std::scoped_lock lock{untile_mutex};
  if (const auto it = untile_pipelines.find(key);
      it != untile_pipelines.end()) {
    return it->second.get();
  }
    const bool macro = AmdGpu::IsMacroTiled(info.array_mode);
    const auto values = VideoCore::TilingSpecialization(info, tiler);
    std::array<Rhi::SpecConstant, 15> constants{};
    for (u32 i = 0; i < constants.size(); ++i) {
        constants[i].id = i;
        constants[i].value = i < values.size() ? values[i] : 0;
    }
    constants[12].value = info.props.is_bgr_packed_float;
    constants[13].value = !info.props.is_tiled;
    constants[14].value = info.props.is_bgr_packed_float;
    const auto code = UntileCode(info.num_bits, macro);
    if (code.empty() || !untile_layout) {
        return nullptr;
    }
    auto pipeline = device.CreateComputePipeline({
        .layout = untile_layout.get(),
        .shader = {Rhi::ShaderStage::Compute, code, "main",
                   std::span<const Rhi::SpecConstant>{constants}},
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

// FIX-204: whether a tiny half-float texture's memory holds no plausible values (NaN, infinity
// or magnitudes from 2048 up) in the texels it has. GT7 samples two 4x4 RGBA16F textures for
// the driver's lighting in its vertex shader that nothing in a session ever writes: on fresh
// memory they hold zeros and the driver looks right, but the allocator often places them over
// memory that held other data (compressed texture blocks, descriptor tables), and the driver
// then lit up in a random colour with a glare over the whole picture, in most launches. Such
// textures upload as zeros. SHADGT_NATIVE_SANITIZE_TINY=0: as memory holds them.
static bool ImplausibleTinyFloatTexture(const ImageEntry& entry) {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADGT_NATIVE_SANITIZE_TINY");
        return !(env && env[0] == '0');
    }();
    const auto& info = entry.info;
    if (!enabled || !entry.from_cpu || info.size.width > 4 || info.size.height > 4 ||
        info.resources.levels != 1 || info.resources.layers != 1 || info.props.is_depth ||
        info.props.is_block) {
        return false;
    }
    u32 halves = 0;
    switch (info.pixel_format) {
    case vk::Format::eR16Sfloat:
        halves = 1;
        break;
    case vk::Format::eR16G16Sfloat:
        halves = 2;
        break;
    case vk::Format::eR16G16B16A16Sfloat:
        halves = 4;
        break;
    default:
        return false;
    }
    // Linear, or thin micro tiles (GT7's are Thin1DThin): within a micro tile, texels of a 4x4
    // corner are in Morton order.
    const bool linear = !info.props.is_tiled;
    if (!linear && info.tile_mode != AmdGpu::TileMode::Thin1DThin) {
        return false;
    }
    std::array<u16, 256> memory{};
    const u64 bytes = std::min<u64>(info.guest_size, sizeof(memory));
    if (!Core::RytyMemory::GuardedCopy(memory.data(),
                                       reinterpret_cast<const void*>(info.guest_address), bytes)) {
        return false;
    }
    const u64 pitch = std::max<u64>(info.pitch, info.size.width);
    for (u32 y = 0; y < info.size.height; ++y) {
        for (u32 x = 0; x < info.size.width; ++x) {
            const u64 index = linear ? y * pitch + x
                                     : (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2);
            if ((index + 1) * halves * 2 > bytes) {
                return false;
            }
            for (u32 h = 0; h < halves; ++h) {
                const u32 exponent = (memory[index * halves + h] >> 10) & 0x1f;
                if (exponent >= 26) { // |x| >= 2048, infinity or NaN
                    return true;
                }
            }
        }
    }
    return false;
}

void ImageRegistry::ReportUploadTally() {
    std::vector<const UploadTally*> top;
    u64 total_bytes = 0, total_uploads = 0;
    for (const auto& [entry, tally] : upload_tally) {
        top.push_back(&tally);
        total_bytes += tally.bytes;
        total_uploads += tally.uploads;
    }
    std::ranges::sort(top, [](const auto* a, const auto* b) { return a->bytes > b->bytes; });
    LOG_INFO(Render_Vulkan, "DIAG-197: {} uploads ({:.1f} MB) of {} pictures since the last report",
             total_uploads, total_bytes / 1048576.0, top.size());
    for (size_t i = 0; i < std::min<size_t>(top.size(), 12); ++i) {
        const auto& t = *top[i];
        LOG_INFO(Render_Vulkan, "DIAG-197:   {:#x} {}x{} {} mips {}: {} uploads, {:.1f} MB, forced "
                 "{}, after CPU write/unmap/GPU write/other {}/{}/{}/{}",
                 t.address, t.width, t.height, vk::to_string(t.format), t.levels, t.uploads,
                 t.bytes / 1048576.0, t.forced, t.why[0], t.why[1], t.why[2], t.why[3]);
    }
    upload_tally.clear();
}

bool ImageRegistry::LazyUploads() {
    // PERF-109: textures marked stale are uploaded when work uses them (MarkStale defers every
    // CPU texture; QueueUploadForUse or RecordUploads' `bound` admits them). Always on: eager
    // queuing re-imported textures while an unmap waited (see MarkStale).
    return true;
}

u32 ImageRegistry::RecordUploads(Rhi::CommandList& list, std::span<ImageEntry* const> bound,
                                 bool trace_slow_calls) {
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
    // PERF-109: the stale textures the work uses (an entry listed twice is uploaded once).
    for (ImageEntry* entry : bound) {
        if (entry && entry->from_cpu && UploadDue(*entry)) {
            pending.push_back(entry);
        }
    }
    // Duplicates may observe needs_upload becoming true again while an earlier
    // snapshot is copied. Consume each image only once, preserving that new dirty
    // state for the next actual use rather than uploading it repeatedly now.
    UniqueUploadBatch(pending);
    u32 uploaded = 0;
    std::vector<std::unique_ptr<Rhi::Buffer>> staging; // freed after the GPU used them
    for (ImageEntry* entry : pending) {
        if (!UploadDue(*entry)) {
            continue; // PERF-GTS-384: not stale, or uploaded this frame already (stays stale)
        }
        if (!entry->needs_upload.exchange(false)) {
            continue;
        }
        entry->upload_frame.store(upload_frame_now.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
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
        {
            // WIDE-006 (diagnosis): a texture made of targets uploaded from guest memory.
            bool made_of_targets;
            {
                std::scoped_lock lock{entry->sources_mutex};
                made_of_targets = !entry->sources.empty();
            }
            static std::atomic<u32> logged{};
            if (made_of_targets && logged.fetch_add(1, std::memory_order_relaxed) < 60) {
                std::fprintf(stderr, "[wide] upload of a crop %#llx %ux%u (forced %d)\n",
                             static_cast<unsigned long long>(info.guest_address), info.size.width,
                             info.size.height, int(entry->force_upload.load()));
            }
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
        // PERF-108: not a texture uploaded because GPU work wrote it: its picture comes from the
        // region the GPU wrote, and its last hash still tells CPU writes apart. Hashing them
        // all read ~3 GB a second of guest memory on the recorder in GT7's races (compute
        // output sampled as textures, written several times a frame).
        static const bool hash_forced = [] {
            const char* env = std::getenv("SHADGT_NATIVE_HASH_FORCED"); // PERF-108 off: =1
            return env && env[0] == '1';
        }();
        const bool skip_hash = forced && entry->from_cpu && !hash_forced;
        if ((size <= HashedSize || !entry->from_cpu) && !skip_hash && memory_clamp &&
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
            // FIX-209: a forced upload that skipped the hash (PERF-108) forgets the old one:
            // kept, a later CPU write that put those older bytes back matched it, was taken
            // for "unchanged" and never uploaded, and the texture kept the forced picture.
            entry->content_hash = 0;
        }
        // (diagnosis) SHADGT_DIAG_ZERO_TEXTURES=<w>x<h>x<bits>: textures of that size upload zeros.
        static const std::array<u32, 3> zero_size = [] {
            std::array<u32, 3> parts{};
            if (const char* env = Common::DiagEnv("SHADGT_DIAG_ZERO_TEXTURES")) {
                std::sscanf(env, "%ux%ux%u", &parts[0], &parts[1], &parts[2]);
            }
            return parts;
        }();
        bool zero_texture = entry->from_cpu && zero_size[0] != 0 &&
                            info.size.width == zero_size[0] && info.size.height == zero_size[1] &&
                            info.num_bits == zero_size[2];
        if (!zero_texture && ImplausibleTinyFloatTexture(*entry)) {
            zero_texture = true; // FIX-204
            ++upload_stats.sanitized;
            static std::atomic<u32> logged{};
            if (logged++ < 16) {
                LOG_INFO(Render_Vulkan, "FIX-204: texture {:#x} {}x{} {} holds no plausible "
                         "values; uploaded as zeros", info.guest_address, info.size.width,
                         info.size.height, vk::to_string(info.pixel_format));
            }
        }
        // NAT-107: the bytes as the GPU sees guest memory, in GPU order (textures the game's
        // GPU work filled, in memory the CPU may not even read), else a copy the CPU makes now.
        // (diagnosis) SHADGT_DIAG_UPLOAD_CPU_SIZE=<w>x<h>: textures that size are copied from guest
        // memory by the CPU, never from a region or an imported view.
        static const std::array<u32, 2> cpu_size = [] {
            std::array<u32, 2> parts{};
            if (const char* env = Common::DiagEnv("SHADGT_DIAG_UPLOAD_CPU_SIZE")) {
                std::sscanf(env, "%ux%u", &parts[0], &parts[1]);
            }
            return parts;
        }();
        const bool cpu_only = cpu_size[0] != 0 && info.size.width == cpu_size[0] &&
                              info.size.height == cpu_size[1];
        trace.Next(4);
        UploadSource gpu_source;
        if (upload_source && !zero_texture && !cpu_only) {
            gpu_source = upload_source(*entry, list);
        }
#ifndef NDEBUG
        // DIAG-200 (diagnosis, SHADGT_DIAG_UPLOAD_SIZE=<w>x<h>): each upload of textures that size,
        // where its bytes come from (1 region, 2 imported view, 3 gathered, 0 CPU copy) and a
        // hash of guest memory at the time.
        static const std::array<u32, 2> diag_size = [] {
            std::array<u32, 2> parts{};
            if (const char* env = Common::DiagEnv("SHADGT_DIAG_UPLOAD_SIZE")) {
                std::sscanf(env, "%ux%u", &parts[0], &parts[1]);
            }
            return parts;
        }();
        if (diag_size[0] != 0 && info.size.width == diag_size[0] &&
            info.size.height == diag_size[1]) {
            static std::atomic<u32> diag_logged{};
            if (diag_logged++ < 100000) {
                std::vector<u8> memory(size);
                const bool read = Core::RytyMemory::GuardedCopy(
                    memory.data(), reinterpret_cast<const void*>(info.guest_address), size);
                u64 nonzero = 0;
                for (const u8 byte : memory) {
                    nonzero += byte != 0;
                }
                LOG_WARNING(Render_Vulkan, "DIAG-200: frame {} texture {:#x}+{:#x} {}x{} layers {} "
                            "source {} forced {} stale {} memory {:016x} ({} nonzero bytes{})",
                            entry->last_use.load(), info.guest_address, size, info.size.width,
                            info.size.height, info.resources.layers, u32(gpu_source.kind), forced,
                            u32(entry->last_stale), read ? XXH3_64bits(memory.data(), size) : 0,
                            nonzero, read ? "" : ", unreadable");
            }
        }
#endif
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
            // A streamed texture can have unmapped leading mips and resident lower mips.
            // A readable prefix is only the fast path, not the extent of its mapped data.
            trace.Next(6);
            const u64 readable =
                zero_texture ? 0 : memory_clamp ? memory_clamp(info.guest_address, size) : size;
            if (readable < size && !zero_texture) {
                static std::atomic<u32> logged{};
                if (logged++ < 16) {
                    LOG_WARNING(Render_Vulkan,
                                "NAT-042: texture {:#x}+{:#x} ({}x{}, {} mips) is mapped for {:#x} "
                                "contiguous bytes; copying its sparse mapped pages",
                                info.guest_address, size, info.size.width, info.size.height,
                                info.resources.levels, readable);
                }
            }
            if (zero_texture) {
                std::memset(source->Mapped(), 0, size);
            } else if (readable != size ||
                       !Core::RytyMemory::GuardedCopy(source->Mapped(),
                                                      reinterpret_cast<const void*>(info.guest_address),
                                                      size)) {
                // FIX-160: backing pages remain readable when the guest view has holes or is
                // GPU-only. Copy the whole range; only genuinely unmapped pages become zeros.
                Core::Memory::Instance()->CopySparseMemory(info.guest_address, source->Mapped(),
                                                           size);
            }
            bytes = source.get();
            bytes_offset = 0;
        }
        const Rhi::Buffer* linear = bytes;
        u64 linear_offset = bytes_offset;
        if (info.props.is_tiled || info.props.is_bgr_packed_float) {
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
            list.Dispatch(static_cast<u32>(VideoCore::NativeTilingGroups(size, info.num_bits / 8)), 1, 1);
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
        if (LayerSamplesEnabled() && entry->from_cpu && info.resources.layers >= 16 && size > HashedSize &&
            size % info.resources.layers == 0 &&
            !entry->gpu_backing_written.load(std::memory_order_acquire) && !zero_texture &&
            memory_clamp && memory_clamp(info.guest_address, size) == size &&
            (source || gpu_source.kind == 2 || gpu_source.kind == 3)) {
            const u64 layer_size = size / info.resources.layers;
            auto& shared = *entry->layer_hash;
            std::scoped_lock layer_lock{shared.mutex};
            shared.state.Capture(info.resources.layers, layer_size,
                [&](u32 layer, u64 offset, u64 bytes) {
                    const u64 at = layer * layer_size + offset;
                    return source ? XXH3_64bits(source->Mapped() + at, bytes) :
                                    Core::HashGuestBytes(info.guest_address + at, bytes);
                }, LayerWholeHashEnabled());
            shared.generation.fetch_add(1, std::memory_order_acq_rel); // PERF-GTS-452
            shared.changed.store(false, std::memory_order_release);
        } else {
            auto& shared = *entry->layer_hash;
            std::scoped_lock layer_lock{shared.mutex};
            shared.state.ClearUpload();
            shared.generation.fetch_add(1, std::memory_order_acq_rel); // PERF-GTS-452
            shared.changed.store(false, std::memory_order_release);
        }
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
        // FIX-218: an upload replaces what the texture's targets had copied in (NAT-104), but those
        // were only copied again once a target was drawn anew (NAT-111). GT7's driver fill
        // rewrites its scratch pool before each pass, and the textures over its post-processing
        // pyramid and the car close-ups were uploaded from that (zero) memory and kept it: black
        // blocks on the car during the race start. They take their targets' pictures again at
        // their next use. SHADGT_NATIVE_SOURCES_AFTER_UPLOAD=1: on.
        static const bool sources_after_upload = [] {
            const char* env = std::getenv("SHADGT_NATIVE_SOURCES_AFTER_UPLOAD");
            return env && env[0] == '1'; // off unless asked: it did not fix what it was made for
        }();
        if (sources_after_upload) {
            bool has_sources;
            {
                std::scoped_lock lock{entry->sources_mutex};
                has_sources = !entry->sources.empty();
            }
            if (has_sources) {
                entry->sources_dirty.store(true, std::memory_order_relaxed);
            }
        }
        ++entry->total_uploads;
#ifndef NDEBUG
        ++entry->upload_count;
        {
            // DIAG-197 (diagnosis)
            auto& tally = upload_tally[entry];
            tally.address = info.guest_address;
            tally.width = info.size.width;
            tally.height = info.size.height;
            tally.levels = info.resources.levels;
            tally.format = info.pixel_format;
            ++tally.uploads;
            tally.bytes += size;
            tally.forced += forced ? 1 : 0;
            ++tally.why[std::min<u32>(entry->last_stale, 3)];
        }
#endif
        entry->last_stale = 255;
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

bool ImageRegistry::BeginDownloads(DownloadBatch& batch, u64 bytes, u64 alignment) {
    batch.alignment = std::max<u64>(alignment, 256);
    batch.used = 0;
    batch.scratch = device.CreateBuffer({.size = bytes,
                                         .usage = Rhi::BufferUsageStorage |
                                                  Rhi::BufferUsageTransferSrc |
                                                  Rhi::BufferUsageTransferDst,
                                         .memory = Rhi::MemoryKind::DeviceLocal,
                                         .name = "native write-back scratch"});
    return batch.scratch != nullptr;
}

void ImageRegistry::FinishDownloads(Rhi::CommandList& list, DownloadBatch& batch) {
    if (batch.tilings.empty()) {
        if (batch.copied) {
            list.Barrier(Rhi::ScopeTransfer, Rhi::ScopeTransfer);
            batch.copied = false;
        }
        return;
    }
    list.Barrier(Rhi::ScopeTransfer, Rhi::ScopeCompute);
    const Rhi::Pipeline* bound = nullptr;
    for (const auto& tiling : batch.tilings) {
        if (tiling.pipeline != bound) {
            list.BindPipeline(tiling.pipeline);
            bound = tiling.pipeline;
        }
        const std::array<Rhi::Binding, 3> bindings{{
            {.slot = 0, .type = Rhi::BindingType::StorageBuffer, .buffer = batch.scratch.get(),
             .offset = tiling.tiled_offset, .size = tiling.size},
            {.slot = 1, .type = Rhi::BindingType::StorageBuffer, .buffer = batch.scratch.get(),
             .offset = tiling.linear_offset, .size = tiling.size},
            {.slot = 2, .type = Rhi::BindingType::UniformBuffer, .buffer = tiling.params,
             .size = sizeof(TilingParams)},
        }};
        list.PushBindings(bindings);
        list.Dispatch(tiling.groups, 1, 1);
    }
    list.Barrier(Rhi::ScopeCompute, Rhi::ScopeTransfer);
    for (const auto& tiling : batch.tilings) {
        list.CopyBuffer(batch.scratch.get(), tiling.tiled_offset, tiling.out, tiling.out_offset,
                        tiling.size);
    }
    list.Barrier(Rhi::ScopeTransfer, Rhi::ScopeTransfer | Rhi::ScopeHost);
    batch.tilings.clear(); // the device frees the temporary buffers after the GPU used them
    batch.copied = false;
}

bool ImageRegistry::RecordDownload(Rhi::CommandList& list, ImageEntry& entry, const Rhi::Buffer& out,
                                   u64 out_offset, DownloadBatch* batch) {
    const VideoCore::ImageInfo& info = entry.info;
    const u64 size = info.guest_size;
    if (size == 0 || out.Size() < out_offset + size) {
        return false;
    }
    if (!info.props.is_tiled && !info.props.is_bgr_packed_float) {
        auto copies = MipCopies(info);
        for (auto& copy : copies) {
            copy.buffer_offset += out_offset;
        }
        list.CopyImageToBuffer(entry.image.get(), &out, copies);
        if (batch) batch->copied = true;
        return true;
    }
    const Rhi::Pipeline* tile = TilingPipeline(info, true);
    // PERF-GTS-370: a batched write-back made a new params buffer each time (PERF-314 reused the
    // picture's only for unbatched ones): 11% of the recorder's time at the Autopolis start went
    // to creating them. The picture's own, made once, is the same bytes; the entry outlives the
    // batch, which FinishDownloads records in the same flush.
    if (batch && tile && !entry.download_params) {
        entry.download_params = TilingParamsBuffer(info);
    }
    const Rhi::Buffer* batch_params = batch && tile ? entry.download_params.get() : nullptr;
    if (batch && tile && batch_params) {
        // The tiling leaves padding unwritten: the copy back keeps what guest memory holds there.
        const u64 step = Common::AlignUp(size, batch->alignment);
        if (batch->used > batch->scratch->Size() ||
            step > (batch->scratch->Size() - batch->used) / 2) {
            return false;
        }
        const u64 linear_offset = batch->used;
        const u64 tiled_offset = linear_offset + step;
        batch->used = tiled_offset + step;
        auto copies = MipCopies(info);
        for (auto& copy : copies) {
            copy.buffer_offset += linear_offset;
        }
        list.CopyImageToBuffer(entry.image.get(), batch->scratch.get(), copies);
        list.CopyBuffer(&out, out_offset, batch->scratch.get(), tiled_offset, size);
        batch->tilings.push_back({.pipeline = tile,
                                  .params = batch_params,
                                  .out = &out,
                                  .out_offset = out_offset,
                                  .size = size,
                                  .linear_offset = linear_offset,
                                  .tiled_offset = tiled_offset,
                                  .groups = static_cast<u32>(VideoCore::NativeTilingGroups(size, info.num_bits / 8))});
        return true;
    }
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
                                                              Rhi::BufferUsageTransferSrc |
                                                              Rhi::BufferUsageTransferDst,
                                                    .memory = Rhi::MemoryKind::DeviceLocal});
    }
    Rhi::Buffer* params_buffer = entry.download_params.get();
    Rhi::Buffer* linear = entry.download_linear.get();
    Rhi::Buffer* tiled = entry.download_tiled.get();
    if (!tile || !params_buffer || !linear || !tiled) {
        return false;
    }
    list.CopyImageToBuffer(entry.image.get(), linear, MipCopies(info));
    if (info.props.is_bgr_packed_float) list.CopyBuffer(&out, out_offset, tiled, 0, size);
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
    list.Dispatch(static_cast<u32>(VideoCore::NativeTilingGroups(size, info.num_bits / 8)), 1, 1);
    list.Barrier(Rhi::ScopeCompute, Rhi::ScopeTransfer);
    list.CopyBuffer(tiled, 0, &out, out_offset, size);
    // Also orders this write-back's reads of the scratch buffers before the next one's writes.
    list.Barrier(Rhi::ScopeTransfer, Rhi::ScopeCompute | Rhi::ScopeHost | Rhi::ScopeTransfer);
    return true;
}

} // namespace Native
