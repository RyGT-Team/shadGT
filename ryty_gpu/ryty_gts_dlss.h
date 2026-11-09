// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <functional>
#include "ryty_gpu/ryty_dlss.h"
#include "ryty_gpu/ryty_fsr3.h"
#include "ryty_gpu/ryty_framegen.h"
#include "ryty_gpu/ryty_xess.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"

namespace Vulkan {
class Instance;
class Runtime;

// Non-owning image access for both texture-cache and Native images. Native callers splice
// their secondary list before use; every Native image is restored to GENERAL on return.
struct GtsSceneImage {
    GtsSceneImage(VideoCore::Image& image);
    GtsSceneImage(const VideoCore::ImageInfo& info, u64 uid, vk::Image image,
                  vk::ImageAspectFlags aspects,
                  std::function<vk::ImageView(vk::Format)> view,
                  std::function<void()> modified);
    const VideoCore::ImageInfo& info;
    u64 image_uid;
    vk::ImageAspectFlags aspect_mask;
    VideoCore::Image* cached{};
    vk::Image raw{};
    std::function<vk::ImageView(vk::Format)> native_view;
    std::function<void()> native_modified;
    VideoCore::Image::State state{};
    vk::Image GetImage() const;
    vk::ImageView View(vk::Format format);
    void MarkGpuModified();
    VideoCore::Image::State State() const;
};

class GtsDlssPass {
public:
    GtsDlssPass(const Instance& instance, Scheduler& scheduler, Runtime& runtime);
    /// Temporal DLSS, or FSR-200 temporal FSR when DLSS is not on.
    bool Enabled() const { return RytyDlss::TemporalEnabled() || useFsr || useXess; }
    void SetOutputSize(vk::Extent2D size, bool hdrEncoded) {
        requestedSize = size;
        supportedDisplay = !hdrEncoded;
    }
    bool Capture(u64 frame, GtsSceneImage color, GtsSceneImage depth,
                 GtsSceneImage motion, GtsSceneImage resolved,
                 const std::array<float, 28>& constants);
    bool HasScene(u64 frame, u64 resolvedUid) const;
    bool CopyFrameGenerationInputs(u64 frame, RytyFrameGen::SceneSnapshot& snapshot,
                                   bool native_hudless = false);
    void PrepareScene(u64 frame, GtsSceneImage resolved);
    void Resolve(u64 frame, GtsSceneImage display);
    bool ResolveNativeComposition(u64 frame, GtsSceneImage scene, GtsSceneImage display);
    /// WIDE-001: the last reconstructed picture (outputSize) into `rect` of `dst`, which is in the
    /// transfer-destination layout. False when there is none.
    bool BlitOutputInto(vk::Image dst, vk::Rect2D rect);
    bool OverrideHud(u64 frame, u64 uid, RenderState& state);
    void FallbackHud(u64 frame, GtsSceneImage display, vk::ImageLayout restoreLayout);
    void SyncGuestRead(u64 frame, GtsSceneImage display);
    void ScaleHud(u64 frame, u64 uid, Viewports& viewports, Scissors& scissors) const;
    vk::ImageView PreparePresent(u64 frame, GtsSceneImage display, vk::Format viewFormat);
    vk::Extent2D OutputSize() const { return outputSize; }
    vk::Extent2D RenderSize() const { return renderSize; }
    /// FSR-204: the temporal upscaler, for the window title.
    std::string BackendName() const;
    RygtDlssQuality ActiveQuality() const { return quality; }
    bool HasDisplay(u64 frame, u64 uid) const { return isActive(frame, uid); }

private:
    struct Texture {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        vk::UniqueImageView presentView;
        vk::Format format{};
        vk::Extent2D size{};
        vk::ImageAspectFlags aspects{vk::ImageAspectFlagBits::eColor};
        vk::ImageLayout layout{vk::ImageLayout::eUndefined};
        RygtDlssImage Descriptor() const;
    };
    void create(Texture& texture, vk::Format format, vk::Extent2D size,
                vk::ImageAspectFlags aspects = vk::ImageAspectFlagBits::eColor);
    void transit(Texture& texture, vk::ImageLayout layout);
    void copyInput(GtsSceneImage source, Texture& dest);
    void transitSource(GtsSceneImage& source, vk::ImageLayout layout,
                       vk::PipelineStageFlags2 stage, vk::AccessFlags2 access);
    void copyBack(GtsSceneImage display);
    void copyDepthForFsr();
    bool isActive(u64 frame, u64 uid) const;

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    Texture color, depth, motion, reconstruction, native;
    std::unique_ptr<VideoCore::BlitHelper> colorCopy;
    vk::Extent2D requestedSize{}, renderSize{}, outputSize{};
    vk::Extent2D rejectedInput{}, rejectedOutput{};
    RygtDlssQuality quality{RYGT_DLSS_QUALITY_PERFORMANCE};
    u64 sceneFrame{~u64{}}, sceneUid{}, displayUid{}, outputFrame{~u64{}};
    float jitterX{}, jitterY{}, frameTimeMs{1000.f / 60};
    bool reset{true}, failed{}, supportedDisplay{}, nativeDirty{};
    bool useFsr{}; // FSR-200: the scene goes to FSR instead of DLSS
    // FSR-205: the scene depth as R32_SFLOAT for FSR (its Vulkan backend views a D32_SFLOAT_S8_UINT
    // image as D32_SFLOAT, an invalid view that lost the device on an RX 7600), through a buffer.
    Texture fsrDepth;
    std::unique_ptr<VideoCore::UniqueBuffer> depthCopy;
    RytyFsr3::Temporal fsr;
    bool useXess{}; // XESS-200: the scene goes to Intel XeSS
    RytyXess::Temporal xess;
    u64 evaluatedFrames{}, nativePresentations{}, guestSyncs{}, nativeHudDraws{}, fallbacks{};
    std::chrono::steady_clock::time_point lastScene{};
};
} // namespace Vulkan
