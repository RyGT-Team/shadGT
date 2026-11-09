// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// UI-HIRES (ryGT): GT Sport's UI drawn at the output size. After a flip, the first draw into a
// display buffer is the scene composite. Before the next draw into it, DLSS (or a linear blit
// when DLSS cannot scale) scales that picture into a proxy image at the output size, and the
// remaining draws into the display buffer (the UI) render into the proxy with their viewport and
// scissor scaled. The presenter then shows the proxy instead of scaling the display buffer.
// A draw the proxy cannot take (more targets, a depth buffer) resolves the proxy back into the
// display buffer and the frame continues the normal way. GPU.ui_hires in
// user\custom_configs\CUSA03220.json (the launcher's option) or RYGT_UI_HIRES=1 turns it on.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "ryty_gpu/ryty_dlss.h"
#include "ryty_gpu/ryty_pro_mode.h"
#include "ryty_gpu/ryty_hdr_ui.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {
// WIDE-001 (ryty_game_patch.cpp): the scene at the screen's shape, the HUD and menus 16:9 in the
// middle of the output.
struct RytySceneExtent {
    std::uint32_t width{}, height{};
};
RytySceneExtent RytySceneSize();
bool RytyWideScene();
bool RytyWideViews();  // WIDE-002/004: the game's 3D views are widened
bool RytyHudSpread();  // WIDE-005: race HUD sides moved to the screen's edges
bool RytyGtsRaceSession(); // WIDE-015: from a race project until the menus proper
bool RytyGtsPhotoMode();   // WIDE-059: the replay's photo mode is open
int RytyGtsMenuScreen();   // WIDE-020: 2 = Home
} // namespace Vulkan

namespace Vulkan::UiHires {

enum class Phase {
    Idle,  ///< nothing drawn into the display buffer since its last flip
    Scene, ///< the scene composite was drawn into the display buffer
    Ui,    ///< the proxy holds the picture; display buffer draws go to the proxy
    Off,   ///< the proxy was given up for this frame; the display buffer holds the picture
};

struct Proxy {
    vk::Extent2D native{}; ///< the display buffer's size as the presenter shows it
    vk::Extent2D size{};   ///< the output (window or screen) size; empty when not used
    vk::Format format{};
    VideoCore::UniqueImage image;
    VideoCore::UniqueImage scaled; ///< DLSS output (R16G16B16A16), copied into image
    vk::UniqueImageView scaled_view;
    std::vector<std::pair<std::pair<vk::Format, bool>, vk::UniqueImageView>> views;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    // HDR-UI: the UI layer (RGBA16F, SDR colours and coverage), composed by the presenter.
    VideoCore::UniqueImage ui;
    vk::UniqueImageView ui_view;
    vk::ImageLayout ui_layout = vk::ImageLayout::eUndefined;
    bool ui_used = false;
    Phase phase = Phase::Idle;
    bool dlss_failed = false;
    bool dlss_used = false;
    bool native_stale = false; ///< the proxy got draws the display buffer has not
    /// WIDE-001: the picture fills the whole proxy (a wide race scene); otherwise the display
    /// buffer's picture is in the 16:9 rectangle in its middle (UiRect).
    bool full = false;
    /// WIDE-005: this frame's race HUD was drawn in pieces moved to the screen's edges.
    bool spread = false;
    /// WIDE-042: the ambient sides' working images (the 16:9 picture halved 1..6 times).
    std::array<VideoCore::UniqueImage, 6> ambient;
    std::array<vk::Extent2D, 6> ambient_size{};
    vk::Format ambient_format{};
    bool ambient_general = false;
};

struct Retired {
    VideoCore::UniqueImage image;
    VideoCore::UniqueImage scaled;
    vk::UniqueImageView scaled_view;
    VideoCore::UniqueImage ui;
    vk::UniqueImageView ui_view;
    std::vector<std::pair<std::pair<vk::Format, bool>, vk::UniqueImageView>> views;
    u64 frame = 0;
    std::array<VideoCore::UniqueImage, 6> ambient; // WIDE-042
};

struct State {
    std::mutex mutex;
    std::unordered_map<VAddr, Proxy> proxies;
    std::deque<Retired> retired; ///< freed some frames later: the GPU may still read them
    u64 frames = 0;
    /// Nonzero while a draw into the proxy is recorded: its viewport and scissor scale.
    float scale = 0.0f;
    /// WIDE-001: the vertical scale and the offset of that mapping (scale is the horizontal one).
    float scale_y = 0.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    /// Lock-free checks for every draw: the display buffer addresses with a proxy, and how
    /// many proxies hold draws their display buffer has not.
    std::array<std::atomic<VAddr>, 8> addresses{};
    std::atomic<u32> stale{};
};

inline State& Get() {
    static State state;
    return state;
}

inline void MarkStale(Proxy& proxy, bool stale) {
    if (proxy.native_stale != stale) {
        proxy.native_stale = stale;
        if (stale) {
            Get().stale.fetch_add(1, std::memory_order_relaxed);
        } else {
            Get().stale.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

inline bool KnownAddress(VAddr address) {
    for (const auto& known : Get().addresses) {
        if (known.load(std::memory_order_relaxed) == address) {
            return true;
        }
    }
    return false;
}

inline bool Enabled() {
    static const bool enabled = [] {
        if (const char* env = std::getenv("RYGT_UI_HIRES")) {
            return env[0] == '1';
        }
        return Ryty::ConfigTrue("ui_hires");
    }();
    return enabled;
}

/// WIDE-005: the race HUD in pieces. The 16:9 HUD is cut into regions (fractions of its width and
/// height): the left column (standings, position, laps; the lap time at the bottom left) moves to
/// the screen's left edge, the right column (map, lap times, course map) to its right edge, and
/// the middle (timer, gauges, messages) stays.
struct HudPiece {
    float u0, v0, u1, v1;
    int side; // -1 to the left edge, 0 stays, 1 to the right edge
};
// The bottom row starts at 70% of the height: the bumper camera's big speedometer and rev
// counter reach up to 73% between 27% and 72% of the width.
constexpr std::array<HudPiece, 6> HudPieces{{
    {0.00f, 0.00f, 0.30f, 0.70f, -1}, // standings, position, laps
    {0.00f, 0.70f, 0.17f, 1.00f, -1}, // lap time, bottom left
    {0.70f, 0.00f, 1.00f, 0.70f, 1},  // map, lap times
    {0.82f, 0.70f, 1.00f, 1.00f, 1},  // course map, brake balance (WIDE-040: was 0.80)
    {0.30f, 0.00f, 0.70f, 0.70f, 0},  // timer, messages
    {0.17f, 0.70f, 0.82f, 1.00f, 0},  // gauges, the overtake gauge (WIDE-040)
}};

/// WIDE-001: wide output with the game's 16:9 picture in the middle.
inline bool Wide() {
    static const bool wide = Enabled() && RytyWideScene();
    return wide;
}


/// The part of the proxy that holds the game's display buffer: all of it, or (wide output) the
/// rectangle of the display buffer's shape in its middle.
inline vk::Rect2D UiRect(const Proxy& proxy) {
    if (!Wide() || proxy.native.width == 0 || proxy.native.height == 0) {
        return vk::Rect2D{{0, 0}, proxy.size};
    }
    u32 width = u32(std::lround(double(proxy.size.height) * proxy.native.width /
                                proxy.native.height));
    u32 height = proxy.size.height;
    if (width > proxy.size.width) {
        width = proxy.size.width;
        height = u32(std::lround(double(proxy.size.width) * proxy.native.height /
                                 proxy.native.width));
    }
    return vk::Rect2D{{s32((proxy.size.width - width) / 2), s32((proxy.size.height - height) / 2)},
                      {width, height}};
}

/// The viewport and scissor mapping of draws into the proxy: game coordinates (the display
/// buffer's) to the proxy's. `full`: to the whole proxy (a wide race scene's composition).
inline void SetMapping(State& state, const Proxy& proxy, bool full) {
    if (full || !Wide()) {
        state.scale = float(proxy.size.width) / float(proxy.native.width);
        state.scale_y = full ? float(proxy.size.height) / float(proxy.native.height) : state.scale;
        state.offset_x = 0.0f;
        state.offset_y = 0.0f;
        return;
    }
    const vk::Rect2D rect = UiRect(proxy);
    state.scale = float(rect.extent.width) / float(proxy.native.width);
    state.scale_y = float(rect.extent.height) / float(proxy.native.height);
    state.offset_x = float(rect.offset.x);
    state.offset_y = float(rect.offset.y);
}

/// The blit region from the display buffer's picture (`from`) into the proxy's part for it.
inline vk::ImageBlit2 IntoProxy(const Proxy& proxy, vk::Extent2D from) {
    const vk::Rect2D rect = UiRect(proxy);
    return vk::ImageBlit2{
        .srcSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{s32(from.width), s32(from.height), 1}},
        .dstSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .dstOffsets = std::array{vk::Offset3D{rect.offset.x, rect.offset.y, 0},
                                 vk::Offset3D{rect.offset.x + s32(rect.extent.width),
                                              rect.offset.y + s32(rect.extent.height), 1}},
    };
}

/// The blit region from the proxy back into the display buffer (`to`): the 16:9 middle, where the
/// HUD is drawn. HUD panels that read the picture behind them (blurred boxes, overlays) then
/// line up with it. (Squeezing a wide race picture whole put a second, narrower copy behind them.)
/// WIDE-005: with the HUD spread, each HUD region of the display buffer gets the part of the
/// output its HUD piece was moved over, so panels that show the picture behind them (the maps'
/// backgrounds) show what is really behind them. Up to HudPieces.size() regions.
inline std::vector<vk::ImageBlit2> FromProxySpread(const Proxy& proxy, vk::Extent2D to);

inline vk::ImageBlit2 FromProxy(const Proxy& proxy, vk::Extent2D to) {
    const vk::Rect2D rect = UiRect(proxy);
    return vk::ImageBlit2{
        .srcSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .srcOffsets = std::array{vk::Offset3D{rect.offset.x, rect.offset.y, 0},
                                 vk::Offset3D{rect.offset.x + s32(rect.extent.width),
                                              rect.offset.y + s32(rect.extent.height), 1}},
        .dstSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{s32(to.width), s32(to.height), 1}},
    };
}

inline void Retire(State& state, Proxy& proxy) {
    if (!proxy.image && !proxy.scaled) {
        return;
    }
    state.retired.push_back(Retired{std::move(proxy.image), std::move(proxy.scaled),
                                    std::move(proxy.scaled_view), std::move(proxy.ui),
                                    std::move(proxy.ui_view), std::move(proxy.views),
                                    state.frames, std::move(proxy.ambient)});
    proxy.ambient_size = {};
    proxy.ambient_general = false;
    proxy.views.clear();
    proxy.layout = vk::ImageLayout::eUndefined;
    proxy.ui_layout = vk::ImageLayout::eUndefined;
}

inline vk::ImageView View(const Instance& instance, Proxy& proxy, vk::Format format,
                          bool alpha_one) {
    const auto key = std::make_pair(format, alpha_one);
    for (const auto& [k, view] : proxy.views) {
        if (k == key) {
            return *view;
        }
    }
    auto view = instance.GetDevice().createImageViewUnique(vk::ImageViewCreateInfo{
        .image = proxy.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .components{.a = alpha_one ? vk::ComponentSwizzle::eOne : vk::ComponentSwizzle::eIdentity},
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    });
    if (view.result != vk::Result::eSuccess) {
        return {};
    }
    const vk::ImageView handle = *view.value;
    proxy.views.emplace_back(key, std::move(view.value));
    return handle;
}

inline bool CreateImages(const Instance& instance, Proxy& proxy, vk::Format format) {
    if (proxy.image && proxy.format == format) {
        return true;
    }
    Retire(Get(), proxy);
    proxy.format = format;
    proxy.image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    proxy.image.Create(vk::ImageCreateInfo{
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent{.width = proxy.size.width, .height = proxy.size.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    proxy.scaled = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    proxy.scaled.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent{.width = proxy.size.width, .height = proxy.size.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    auto view = instance.GetDevice().createImageViewUnique(vk::ImageViewCreateInfo{
        .image = proxy.scaled,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    });
    if (view.result != vk::Result::eSuccess) {
        return false;
    }
    proxy.scaled_view = std::move(view.value);
    proxy.layout = vk::ImageLayout::eUndefined;
    proxy.ui = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    proxy.ui.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent{.width = proxy.size.width, .height = proxy.size.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    auto ui_view = instance.GetDevice().createImageViewUnique(vk::ImageViewCreateInfo{
        .image = proxy.ui,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    });
    if (ui_view.result != vk::Result::eSuccess) {
        return false;
    }
    proxy.ui_view = std::move(ui_view.value);
    proxy.ui_layout = vk::ImageLayout::eUndefined;
    LOG_INFO(Render_Vulkan, "UI-HIRES: proxy {}x{} for a {}x{} display buffer, {}",
             proxy.size.width, proxy.size.height, proxy.native.width, proxy.native.height,
             vk::to_string(format));
    return true;
}

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .levelCount = 1,
    .layerCount = 1,
};

inline void ProxyBarrier(vk::CommandBuffer cmdbuf, Proxy& proxy, vk::ImageLayout layout,
                         vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = proxy.layout,
        .newLayout = layout,
        .image = proxy.image,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    });
    proxy.layout = layout;
}

inline std::vector<vk::ImageBlit2> FromProxySpread(const Proxy& proxy, vk::Extent2D to) {
    std::vector<vk::ImageBlit2> regions;
    if (!proxy.spread) {
        regions.push_back(FromProxy(proxy, to));
        return regions;
    }
    const vk::Rect2D rect = UiRect(proxy);
    const float w = float(rect.extent.width), h = float(rect.extent.height);
    for (const auto& piece : HudPieces) {
        const s32 dx = piece.side * rect.offset.x;
        const s32 sx0 = rect.offset.x + s32(std::lround(piece.u0 * w)) + dx;
        const s32 sx1 = rect.offset.x + s32(std::lround(piece.u1 * w)) + dx;
        const s32 sy0 = rect.offset.y + s32(std::lround(piece.v0 * h));
        const s32 sy1 = rect.offset.y + s32(std::lround(piece.v1 * h));
        const s32 dx0 = s32(std::lround(piece.u0 * to.width));
        const s32 dx1 = s32(std::lround(piece.u1 * to.width));
        const s32 dy0 = s32(std::lround(piece.v0 * to.height));
        const s32 dy1 = s32(std::lround(piece.v1 * to.height));
        if (sx1 <= sx0 || sy1 <= sy0 || dx1 <= dx0 || dy1 <= dy0) {
            continue;
        }
        regions.push_back(vk::ImageBlit2{
            .srcSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
            .srcOffsets = std::array{vk::Offset3D{sx0, sy0, 0}, vk::Offset3D{sx1, sy1, 1}},
            .dstSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
            .dstOffsets = std::array{vk::Offset3D{dx0, dy0, 0}, vk::Offset3D{dx1, dy1, 1}},
        });
    }
    return regions;
}

/// WIDE-001: the proxy black (the bars beside the 16:9 picture), left in the transfer layout for
/// a blit into it.
inline void ClearProxyForBlit(vk::CommandBuffer cmdbuf, Proxy& proxy) {
    ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eTransferDstOptimal,
                 vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite);
    cmdbuf.clearColorImage(proxy.image, vk::ImageLayout::eTransferDstOptimal,
                           vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}}, ColorRange);
    ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eTransferDstOptimal,
                 vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
}

constexpr vk::PipelineStageFlags2 DrawStages =
    vk::PipelineStageFlagBits2::eColorAttachmentOutput |
    vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eVertexShader;
constexpr vk::AccessFlags2 DrawAccess = vk::AccessFlagBits2::eColorAttachmentRead |
                                        vk::AccessFlagBits2::eColorAttachmentWrite |
                                        vk::AccessFlagBits2::eShaderSampledRead;

/// HDR-UI: the UI layer starts each frame transparent.
inline void ClearUiLayer(vk::CommandBuffer cmdbuf, Proxy& proxy) {
    const vk::ImageMemoryBarrier2 to_clear{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eClear,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = proxy.ui,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_clear,
    });
    cmdbuf.clearColorImage(proxy.ui, vk::ImageLayout::eGeneral,
                           vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 0.0f}}, ColorRange);
    const vk::ImageMemoryBarrier2 to_draw{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = DrawStages,
        .dstAccessMask = DrawAccess,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = proxy.ui,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_draw,
    });
    proxy.ui_layout = vk::ImageLayout::eGeneral;
    proxy.ui_used = false;
}

/// HDR-UI: a frame whose first draw is UI: the scene is black.
inline void StartWithoutScene(Scheduler& scheduler, Proxy& proxy) {
    proxy.dlss_used = false;
    proxy.full = false;
    scheduler.EndRendering();
    const vk::CommandBuffer cmdbuf = scheduler.DirectCommandBuffer();
    ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eClear,
                 vk::AccessFlagBits2::eTransferWrite);
    cmdbuf.clearColorImage(proxy.image, vk::ImageLayout::eGeneral,
                           vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}}, ColorRange);
    ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eGeneral, DrawStages, DrawAccess);
    ClearUiLayer(cmdbuf, proxy);
    proxy.phase = Phase::Ui;
}

/// The scene composite in the display buffer `image` becomes the proxy's picture.
inline void Upscale(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                    VideoCore::Image& image, const VideoCore::ImageViewInfo& target_view,
                    Proxy& proxy) {
    scheduler.EndRendering();
    proxy.full = false;
    const bool dlss =
        !Wide() && !proxy.dlss_failed && RytyDlss::CanScale(proxy.native, proxy.size);
    if (dlss) {
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
    } else {
        runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer,
                        vk::AccessFlagBits2::eTransferRead);
    }
    runtime.FlushBarriers();
    const vk::CommandBuffer cmdbuf = scheduler.DirectCommandBuffer();
    bool scaled = false;
    if (dlss) {
        VideoCore::ImageViewInfo info{};
        info.format = target_view.format;
        const vk::ImageView input = *image.FindView(info).image_view;
        scaled = RytyDlss::ScaleInto(cmdbuf, image.GetImage(), input, info.format, proxy.native,
                                     proxy.scaled, *proxy.scaled_view, proxy.size);
        if (!scaled) {
            proxy.dlss_failed = true;
        }
    }
    proxy.dlss_used = scaled;
    if (Wide()) {
        ClearProxyForBlit(cmdbuf, proxy); // WIDE-001: black beside the 16:9 picture
    } else {
        ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eTransferDstOptimal,
                     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    }
    const vk::ImageBlit2 region = IntoProxy(proxy, scaled ? proxy.size : proxy.native);
    if (scaled) {
        const vk::ImageMemoryBarrier2 to_copy{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .image = proxy.scaled,
            .subresourceRange = ColorRange,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_copy,
        });
        cmdbuf.blitImage2(vk::BlitImageInfo2{
            .srcImage = proxy.scaled,
            .srcImageLayout = vk::ImageLayout::eTransferSrcOptimal,
            .dstImage = proxy.image,
            .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
            .regionCount = 1,
            .pRegions = &region,
            .filter = vk::Filter::eNearest,
        });
    } else {
        if (dlss) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer,
                            vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
        }
        cmdbuf.blitImage2(vk::BlitImageInfo2{
            .srcImage = image.GetImage(),
            .srcImageLayout = vk::ImageLayout::eTransferSrcOptimal,
            .dstImage = proxy.image,
            .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
            .regionCount = 1,
            .pRegions = &region,
            .filter = vk::Filter::eLinear,
        });
    }
    ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eGeneral, DrawStages, DrawAccess);
    ClearUiLayer(cmdbuf, proxy);
    proxy.phase = Phase::Ui;
}

/// The proxy's picture goes back into the display buffer; the frame continues without it.
/// With `keep`, the proxy stays in use: the display buffer only gets a copy for draws that
/// read it (they sample in the game's coordinates).
inline void Resolve(Scheduler& scheduler, Runtime& runtime, VideoCore::Image& image,
                    Proxy& proxy, bool keep = false) {
    scheduler.EndRendering();
    runtime.Transit(&image, vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    runtime.FlushBarriers();
    const vk::CommandBuffer cmdbuf = scheduler.DirectCommandBuffer();
    ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eTransferSrcOptimal,
                 vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    const vk::ImageBlit2 region = FromProxy(proxy, proxy.native); // WIDE-001
    cmdbuf.blitImage2(vk::BlitImageInfo2{
        .srcImage = proxy.image,
        .srcImageLayout = vk::ImageLayout::eTransferSrcOptimal,
        .dstImage = image.GetImage(),
        .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
        .regionCount = 1,
        .pRegions = &region,
        .filter = vk::Filter::eLinear,
    });
    ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eGeneral, DrawStages, DrawAccess);
    MarkStale(proxy, false);
    if (!keep) {
        proxy.phase = Phase::Off;
    }
}

struct Redirect {
    Proxy* proxy = nullptr;
    VideoCore::ImageId image_id{};
    u32 cb = 0;
    bool ui = false;   ///< HDR-UI: into the UI layer
    bool skip = false; ///< HDR-UI: a UI-layer draw with no layer to draw into
};

/// Draws that read a display buffer whose proxy holds newer draws read it in the game's
/// coordinates: the proxy is copied into the display buffer first, and stays in use.
template <typename Ctx>
bool RefreshSampled(Ctx& ctx, State& state, Scheduler& scheduler, Runtime& runtime,
                    VideoCore::TextureCache& texture_cache) {
    bool refreshed = false;
    for (const auto& bound : ctx.bound_textures) {
        auto& image = texture_cache.GetImage(bound.image_id);
        const auto found = state.proxies.find(image.info.guest_address);
        if (found != state.proxies.end() && found->second.phase == Phase::Ui &&
            found->second.native_stale) {
            Resolve(scheduler, runtime, image, found->second, true);
            refreshed = true;
        }
    }
    return refreshed;
}

/// Before a compute dispatch: the same for display buffers it reads.
template <typename Ctx>
void BeforeDispatch(Ctx& ctx, Scheduler& scheduler, Runtime& runtime,
                    VideoCore::TextureCache& texture_cache) {
    if (!Enabled() || Get().stale.load(std::memory_order_relaxed) == 0) {
        return;
    }
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    if (state.proxies.empty() || !RefreshSampled(ctx, state, scheduler, runtime, texture_cache)) {
        return;
    }
    // The dispatch's image layouts were set before the copy: set them again for what it binds.
    for (const auto& bound : ctx.bound_textures) {
        auto& image = texture_cache.GetImage(bound.image_id);
        if (!state.proxies.contains(image.info.guest_address)) {
            continue;
        }
        runtime.Transit(&image, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        ctx.image_infos[bound.descriptor_index].imageLayout = vk::ImageLayout::eGeneral;
    }
    runtime.FlushBarriers();
}

/// Before a draw's targets are prepared: whether it renders into a proxy. `ctx` is the
/// rasterizer's draw context (cb_descs, db_desc).
template <typename Ctx>
Redirect Prepare(Ctx& ctx, u32 num_color, bool ui_layer, const Instance& instance,
                 Scheduler& scheduler, Runtime& runtime, VideoCore::TextureCache& texture_cache,
                 bool external_display = false) {
    auto& state = Get();
    state.scale = 0.0f;
    if (!Enabled()) {
        return Redirect{.skip = ui_layer};
    }
    if (external_display) {
        std::scoped_lock lock{state.mutex};
        const auto id = ctx.cb_descs[0].image_id;
        if (id) {
            const auto found = state.proxies.find(texture_cache.GetImage(id).info.guest_address);
            if (found != state.proxies.end()) {
                found->second.phase = Phase::Off;
                MarkStale(found->second, false);
            }
        }
        return Redirect{.skip = ui_layer};
    }
    if (state.stale.load(std::memory_order_relaxed) == 0) {
        // Most draws neither target nor read a display buffer: no lock for them.
        bool display = false;
        for (u32 cb = 0; cb < num_color && !display; ++cb) {
            const auto& [image_id, desc] = ctx.cb_descs[cb];
            display = image_id && KnownAddress(texture_cache.GetImage(image_id).info.guest_address);
        }
        if (!display) {
            return Redirect{.skip = ui_layer};
        }
    }
    std::scoped_lock lock{state.mutex};
    if (state.proxies.empty()) {
        return Redirect{.skip = ui_layer};
    }
    RefreshSampled(ctx, state, scheduler, runtime, texture_cache);
    for (u32 cb = 0; cb < num_color; ++cb) {
        const auto& [image_id, desc] = ctx.cb_descs[cb];
        if (!image_id) {
            continue;
        }
        auto& image = texture_cache.GetImage(image_id);
        const auto found = state.proxies.find(image.info.guest_address);
        if (found == state.proxies.end()) {
            continue;
        }
        auto& proxy = found->second;
        if (proxy.size.width == 0) {
            return Redirect{.skip = ui_layer};
        }
        if (ui_layer) {
            // HDR-UI: UI goes to the UI layer; its pipeline only fits that layer.
            if (num_color != 1 || ctx.db_desc.first || proxy.phase == Phase::Off ||
                !CreateImages(instance, proxy, image.info.pixel_format)) {
                return Redirect{.skip = true};
            }
            if (proxy.phase == Phase::Idle) {
                StartWithoutScene(scheduler, proxy);
            } else if (proxy.phase == Phase::Scene) {
                Upscale(instance, scheduler, runtime, image, desc.view_info, proxy);
            }
            proxy.ui_used = true;
            return Redirect{&proxy, image_id, cb, true};
        }
        switch (proxy.phase) {
        case Phase::Idle:
            proxy.phase = Phase::Scene;
            return {};
        case Phase::Off:
            return {};
        case Phase::Scene:
            if (num_color != 1 || ctx.db_desc.first) {
                proxy.phase = Phase::Off;
                return {};
            }
            if (!CreateImages(instance, proxy, image.info.pixel_format)) {
                proxy.size = {};
                proxy.phase = Phase::Off;
                return {};
            }
            Upscale(instance, scheduler, runtime, image, desc.view_info, proxy);
            break;
        case Phase::Ui:
            if (num_color != 1 || ctx.db_desc.first) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    LOG_WARNING(Render_Vulkan,
                                "UI-HIRES: a draw with {} targets and depth {} into the display "
                                "buffer; the frame goes on without the proxy",
                                num_color, bool(ctx.db_desc.first));
                }
                Resolve(scheduler, runtime, image, proxy);
                return {};
            }
            break;
        }
        MarkStale(proxy, true);
        return Redirect{&proxy, image_id, cb};
    }
    return {};
}

/// After the draw's targets are prepared: it renders into the proxy. Reads of the display buffer
/// stay on the display buffer (Prepare copied the proxy into it).
template <typename Ctx>
void Apply(const Redirect& redirect, Ctx& ctx, RenderState& render_state,
           const Instance& instance) {
    if (!redirect.proxy) {
        return;
    }
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    auto& proxy = *redirect.proxy;
    const auto& desc = ctx.cb_descs[redirect.cb].desc;
    auto& attachment = render_state.color_attachments[redirect.cb];
    attachment.image_view = redirect.ui ? *proxy.ui_view
                                        : View(instance, proxy, desc.view_info.format, false);
    attachment.image_layout = vk::ImageLayout::eGeneral;
    render_state.width = u16(proxy.size.width);
    render_state.height = u16(proxy.size.height);
    SetMapping(state, proxy, false);
}

struct Shown {
    vk::ImageView view;
    vk::Extent2D size;
    vk::ImageView ui; ///< HDR-UI: the UI layer to compose over the scene, or null
    bool dlss = false;
    /// WIDE-042: the sides hold ambient light: outside x0..x1 (fractions of the width) dimmed.
    float side_x0 = 0.0f, side_x1 = 1.0f;
};

/// WIDE-042: the sides of a menu frame (the game's 16:9 picture in the middle, nothing drawn
/// beside it) filled with ambient light: the picture enlarged twice around its centre, so each
/// side continues its own edge, and blurred heavily. The picture is halved six times (each
/// halving averages 2x2 pixels) into working images of its own (no neighbours to bleed from at
/// their borders), then enlarged back step by step (bilinear) and the outer quarters of the
/// result stretched over the sides. Called at the flip, so every screen gets it however it was
/// built, and nothing the game draws later can show the working images.
inline void Ambient(const Instance& instance, vk::CommandBuffer cmdbuf, Proxy& proxy) {
    const vk::Rect2D rect = UiRect(proxy);
    const s32 ox = rect.offset.x, mw = s32(rect.extent.width), mh = s32(rect.extent.height);
    if (ox < 16 || mw < 256 || mh < 128) {
        return;
    }
    if (proxy.ambient_format != proxy.format || proxy.ambient_size[0].width != u32(mw / 2) ||
        proxy.ambient_size[0].height != u32(mh / 2)) {
        u32 w = u32(mw), h = u32(mh);
        for (size_t i = 0; i < proxy.ambient.size(); ++i) {
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
            proxy.ambient[i] = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
            proxy.ambient[i].Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = proxy.format,
                .extent{.width = w, .height = h, .depth = 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .usage = vk::ImageUsageFlagBits::eTransferSrc |
                         vk::ImageUsageFlagBits::eTransferDst,
                .initialLayout = vk::ImageLayout::eUndefined,
            });
            proxy.ambient_size[i] = vk::Extent2D{w, h};
        }
        proxy.ambient_format = proxy.format;
        proxy.ambient_general = false;
    }
    const auto barrier = [&] {
        const vk::MemoryBarrier2 memory{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                                   .pMemoryBarriers = &memory});
    };
    if (!proxy.ambient_general) {
        std::array<vk::ImageMemoryBarrier2, 6> to_general{};
        for (size_t i = 0; i < to_general.size(); ++i) {
            to_general[i] = vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead |
                                 vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = proxy.ambient[i],
                .subresourceRange = ColorRange,
            };
        }
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = u32(to_general.size()),
            .pImageMemoryBarriers = to_general.data(),
        });
        proxy.ambient_general = true;
    }
    ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                 vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite);
    const auto blit = [&](vk::Image src, std::array<s32, 4> from, vk::Image dst,
                          std::array<s32, 4> to) {
        const vk::ImageBlit2 region{
            .srcSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
            .srcOffsets = std::array{vk::Offset3D{from[0], from[1], 0},
                                     vk::Offset3D{from[2], from[3], 1}},
            .dstSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
            .dstOffsets = std::array{vk::Offset3D{to[0], to[1], 0}, vk::Offset3D{to[2], to[3], 1}},
        };
        cmdbuf.blitImage2(vk::BlitImageInfo2{
            .srcImage = src,
            .srcImageLayout = vk::ImageLayout::eGeneral,
            .dstImage = dst,
            .dstImageLayout = vk::ImageLayout::eGeneral,
            .regionCount = 1,
            .pRegions = &region,
            .filter = vk::Filter::eLinear,
        });
        barrier();
    };
    const auto whole = [&](size_t i) {
        return std::array<s32, 4>{0, 0, s32(proxy.ambient_size[i].width),
                                  s32(proxy.ambient_size[i].height)};
    };
    // Down: the middle picture, then each level halved.
    blit(proxy.image, {ox, rect.offset.y, ox + mw, rect.offset.y + mh}, proxy.ambient[0],
         whole(0));
    for (size_t i = 1; i < proxy.ambient.size(); ++i) {
        blit(proxy.ambient[i - 1], whole(i - 1), proxy.ambient[i], whole(i));
    }
    // Up: each level enlarged into the one above it (each step a bilinear filter).
    for (size_t i = proxy.ambient.size() - 1; i > 0; --i) {
        blit(proxy.ambient[i], whole(i), proxy.ambient[i - 1], whole(i - 1));
    }
    // The picture enlarged twice around its centre: the left side shows its left quarter's
    // middle half of the height, the right side its right quarter's.
    const s32 lw = s32(proxy.ambient_size[0].width), lh = s32(proxy.ambient_size[0].height);
    const s32 quarter = std::max(lw * ox / (2 * mw), 1);
    const s32 y0 = lh / 4, y1 = lh - lh / 4;
    const s32 top = rect.offset.y, bottom = rect.offset.y + mh;
    blit(proxy.ambient[0], {0, y0, quarter, y1}, proxy.image, {0, top, ox, bottom});
    blit(proxy.ambient[0], {lw - quarter, y0, lw, y1}, proxy.image,
         {ox + mw, top, s32(proxy.size.width), bottom});
}

/// At a flip of the display buffer at `address`: the proxy to show instead, if it holds the
/// picture. Also sizes the proxy for the next frame from the display buffer and output sizes.
inline std::optional<Shown> TakeFrame(const Instance& instance, vk::CommandBuffer cmdbuf,
                                      VAddr address, vk::Extent2D native, vk::Extent2D output,
                                      vk::Format view_format, vk::Image display = {},
                                      vk::ImageLayout display_layout = vk::ImageLayout::eUndefined) {
    if (!Enabled()) {
        return std::nullopt;
    }
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    ++state.frames;
    while (!state.retired.empty() && state.frames - state.retired.front().frame > 8) {
        state.retired.pop_front();
    }
    auto& proxy = state.proxies[address];
    if (!KnownAddress(address)) {
        for (auto& known : state.addresses) {
            VAddr empty = 0;
            if (known.compare_exchange_strong(empty, address)) {
                break;
            }
        }
    }
    std::optional<Shown> shown;
    if (proxy.phase == Phase::Ui && proxy.image && proxy.size == output &&
        proxy.native == native) {
        // WIDE-042: a menu frame (not the race session): ambient sides. (WIDE-045, the race
        // session's own screens too, was tried and taken back: race screens stay as they were.)
        const bool ambient = Wide() && !proxy.full && !proxy.spread && !RytyGtsRaceSession();
        if (ambient) {
            Ambient(instance, cmdbuf, proxy);
        }
        ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eShaderReadOnlyOptimal,
                     vk::PipelineStageFlagBits2::eFragmentShader |
                         vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderRead);
        shown = Shown{View(instance, proxy, view_format, true), proxy.size, {}, proxy.dlss_used};
        if (ambient) {
            const vk::Rect2D rect = UiRect(proxy);
            shown->side_x0 = float(rect.offset.x) / float(proxy.size.width);
            shown->side_x1 = float(rect.offset.x + s32(rect.extent.width)) / float(proxy.size.width);
        }
        if (proxy.ui_used && proxy.ui) {
            const vk::ImageMemoryBarrier2 to_read{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = proxy.ui_layout,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = proxy.ui,
                .subresourceRange = ColorRange,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &to_read,
            });
            proxy.ui_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            shown->ui = *proxy.ui_view;
        }
    }
    // WIDE-042: a menu frame the game drew no UI over (viewing a car, helmet or suit): its
    // picture goes into the proxy's middle and the sides get ambient light, as with UI.
    // WIDE-059: and photo mode's settings pages (Camera, Effects): the game draws them 16:9 with a
    // depth buffer, which the wide picture cannot take (it is given up for the frame, Off), so the
    // finished page goes into the middle the same way and the sides get ambient light. The
    // preview itself cannot be 32:9: the game renders it for its box (1380x946 of the page).
    const bool photo_page = proxy.phase == Phase::Off && RytyGtsPhotoMode();
    if (!shown && display && (proxy.phase == Phase::Scene || photo_page) && proxy.image &&
        proxy.size == output && proxy.native == native && Wide() &&
        (!RytyGtsRaceSession() || photo_page)) {
        const bool general = display_layout == vk::ImageLayout::eGeneral;
        const auto display_barrier = [&](vk::ImageLayout from, vk::ImageLayout to) {
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                 vk::AccessFlagBits2::eMemoryWrite,
                .oldLayout = from,
                .newLayout = to,
                .image = display,
                .subresourceRange = ColorRange,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                       .pImageMemoryBarriers = &barrier});
        };
        const vk::ImageLayout src_layout =
            general ? vk::ImageLayout::eGeneral : vk::ImageLayout::eTransferSrcOptimal;
        display_barrier(display_layout, src_layout);
        ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eTransferDstOptimal,
                     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
        const auto region = IntoProxy(proxy, native);
        cmdbuf.blitImage2(vk::BlitImageInfo2{
            .srcImage = display,
            .srcImageLayout = src_layout,
            .dstImage = proxy.image,
            .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
            .regionCount = 1,
            .pRegions = &region,
            .filter = vk::Filter::eLinear,
        });
        display_barrier(src_layout, display_layout);
        Ambient(instance, cmdbuf, proxy);
        ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eShaderReadOnlyOptimal,
                     vk::PipelineStageFlagBits2::eFragmentShader |
                         vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderRead);
        shown = Shown{View(instance, proxy, view_format, true), proxy.size, {}, false};
        const vk::Rect2D rect = UiRect(proxy);
        shown->side_x0 = float(rect.offset.x) / float(proxy.size.width);
        shown->side_x1 = float(rect.offset.x + s32(rect.extent.width)) / float(proxy.size.width);
        static std::atomic<u32> logged{};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 3) {
            LOG_INFO(Render_Vulkan, "WIDE-042: a frame without UI shown with ambient sides");
        }
    }
    proxy.phase = Phase::Idle;
    proxy.full = false;
    proxy.spread = false;
    MarkStale(proxy, false);
    // Only an upscale of the same shape: a bigger window or screen of the game's aspect ratio.
    // WIDE-001: or a wider output, the game's picture in its middle (or the wide race scene).
    const bool same_shape = output.width > native.width && output.height > native.height &&
                            std::abs(s64(output.width) * native.height -
                                     s64(output.height) * native.width) <= s64(native.height);
    const bool wide = Wide() && output.width >= native.width && output.height >= native.height &&
                      s64(output.width) * native.height > s64(output.height) * native.width;
    const bool wanted = same_shape || wide;
    const vk::Extent2D size = wanted ? output : vk::Extent2D{};
    Ryty::g_ui_layer_ready.store(wanted, std::memory_order_relaxed); // HDR-UI
    if (proxy.size != size || proxy.native != native) {
        Retire(state, proxy);
        proxy.size = size;
        proxy.native = native;
        proxy.dlss_failed = false;
    }
    return shown;
}

/// Viewport and scissor of a draw into the proxy.
inline void ScaleViewport(vk::Viewport& viewport, vk::Rect2D& scissor) {
    const float scale = Get().scale;
    if (scale == 0.0f) {
        return;
    }
    // WIDE-001: separate vertical scale and an offset (the 16:9 middle of a wide output).
    const auto& state = Get();
    const float scale_y = state.scale_y != 0.0f ? state.scale_y : scale;
    const float ox = state.offset_x;
    const float oy = state.offset_y;
    viewport.x = viewport.x * scale + ox;
    viewport.y = viewport.y * scale_y + oy;
    viewport.width *= scale;
    viewport.height *= scale_y;
    const s64 x0 = s64(std::floor(scissor.offset.x * scale + ox));
    const s64 y0 = s64(std::floor(scissor.offset.y * scale_y + oy));
    const s64 x1 = s64(std::ceil((s64(scissor.offset.x) + scissor.extent.width) * scale + ox));
    const s64 y1 = s64(std::ceil((s64(scissor.offset.y) + scissor.extent.height) * scale_y + oy));
    scissor.offset.x = s32(std::clamp<s64>(x0, 0, 32767));
    scissor.offset.y = s32(std::clamp<s64>(y0, 0, 32767));
    scissor.extent.width = u32(std::clamp<s64>(x1 - scissor.offset.x, 0, 32767));
    scissor.extent.height = u32(std::clamp<s64>(y1 - scissor.offset.y, 0, 32767));
}

} // namespace Vulkan::UiHires
