// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {
class Instance;
}

namespace Vulkan::RytyFrameGen {

// Owned by one presentation Frame. Its ready semaphore orders these copies before
// FG preparation, and its present fence protects reuse/reallocation.
struct SceneInputs {
    vk::Image depth{}, motion{}, hudless{};
    vk::ImageView depth_view{}, motion_view{}, hudless_view{};
    vk::Extent2D hudless_size{};
    vk::Format hudless_format{};
    vk::Extent2D size{};
    float jitter_x{}, jitter_y{}, motion_scale_x{}, motion_scale_y{};
    float frame_time_ms{};
    u64 sequence{};
    bool valid{};
};
struct SceneSnapshot {
    VideoCore::UniqueImage depth, motion;
    vk::UniqueImageView depth_view, motion_view;
    vk::Extent2D size{};
    bool initialized{};
    SceneInputs inputs{};
    VideoCore::UniqueImage hudless_scene, hudless_output;
    vk::UniqueImageView hudless_scene_view, hudless_output_view;
    vk::Extent2D hudless_scene_size{}, hudless_output_size{};
    vk::Format hudless_output_format{};
    bool hudless_scene_initialized{}, hudless_output_initialized{};
    bool hudless_scene_valid{};
};

/// ryGT: AMD FSR 3.1 frame generation (amd_fidelityfx_vk.dll, GPU.framegen_enabled). FidelityFX
/// replaces the swapchain with its own, which shows a generated frame between each pair of game
/// frames from its own present thread. Captured scene depth/motion are supplied through
/// per-presentation snapshots; scenes without matching inputs retain optical-flow fallback.
class FrameGen {
public:
    ~FrameGen();

    /// Creates the swapchain through FidelityFX. False when frame generation is off or
    /// unavailable; the caller then creates a normal swapchain.
    bool CreateSwapchain(const Instance& instance, const vk::SwapchainCreateInfoKHR& info,
                         vk::SwapchainKHR& swapchain);
    /// Destroys the swapchain and both FidelityFX contexts (the device must be idle).
    void Destroy();

    bool Active() const {
        return swapchain_context != nullptr;
    }

    vk::Result GetImages(vk::Device device, vk::SwapchainKHR swapchain,
                         std::vector<vk::Image>& images) const;
    vk::Result Acquire(vk::Device device, vk::SwapchainKHR swapchain, vk::Semaphore semaphore,
                       u32* image_index) const;
    vk::Result Present(vk::Queue queue, const vk::PresentInfoKHR& info) const;

    /// Per presented game frame, before the frame's command buffer is submitted: configures
    /// frame generation and records its preparation.
    void Prepare(vk::CommandBuffer cmdbuf, vk::SwapchainKHR swapchain, vk::Extent2D size,
                 bool hdr, const SceneInputs* scene = nullptr, bool fresh_frame = true);

private:
    void CreateAuxImages(vk::CommandBuffer cmdbuf, vk::Extent2D size);

    vk::Device device{};
    VmaAllocator allocator{};
    void* swapchain_context{};
    void* framegen_context{};
    void* create_swapchain{};
    void* destroy_swapchain{};
    PFN_vkGetSwapchainImagesKHR get_images{};
    PFN_vkAcquireNextImageKHR acquire{};
    PFN_vkQueuePresentKHR present{};
    void* present_count{}; // PFN_getLastPresentCountFFXAPI
    u64 logged_presents{};
    u64 logged_frames{};
    std::chrono::steady_clock::time_point last_log{};
    vk::Extent2D display_size{};
    vk::Format format{};
    bool hdr_output{};
    u64 frame_id{};
    std::chrono::steady_clock::time_point last_prepare{};
    vk::Extent2D aux_size{};
    VideoCore::UniqueImage motion_vectors;
    VideoCore::UniqueImage depth;
};

/// True when GPU.framegen_enabled is on (the renderer then asks for extra queues).
bool Requested();

/// Frames shown so far (game and generated), for the FPS counter; 0 while generation is off.
u64 ShownFrames();

/// True only after recent extra presented frames were observed, not merely enabled.
bool Working();
void ReportWorking(bool working);

// Optional bounded diagnostics; stage 0 snapshots, stage 1 HUD-reference conversion.
int BeginGpuTiming(vk::CommandBuffer cmd, u64 sequence, u32 stage);
void EndGpuTiming(vk::CommandBuffer cmd, int query);


} // namespace Vulkan::RytyFrameGen
