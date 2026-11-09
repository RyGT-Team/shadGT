// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/debug.h"
#include "common/guest_clock.h"
#include "common/thread.h"
#include "common/perf_monitor.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "common/nvtx.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/fps_lock.h"
#include "core/libraries/videoout/videoout_error.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "ryty_gpu/ryty_diag.h"
#ifdef RYTY_GPU
#include "ryty_gpu/ryty_pro_mode.h"
#endif

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;
#ifdef RYTY_GPU
namespace Vulkan { bool NativeModeRequested(); }
#endif

namespace Libraries::VideoOut {

constexpr static bool Is32BppPixelFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8R8G8B8Srgb:
    case PixelFormat::A8B8G8R8Srgb:
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return true;
    default:
        return false;
    }
}

constexpr u32 PixelFormatBpp(PixelFormat pixel_format) {
    switch (pixel_format) {
    case PixelFormat::A16R16G16B16Float:
        return 8;
    default:
        return 4;
    }
}

VideoOutDriver::VideoOutDriver(u32 width, u32 height) {
    Common::FrameQueueTrace::Initialize();
    main_port.resolution.full_width = width;
    main_port.resolution.full_height = height;
    main_port.resolution.pane_width = width;
    main_port.resolution.pane_height = height;
    present_thread = std::jthread([&](std::stop_token token) { PresentThread(token); });
}

VideoOutDriver::~VideoOutDriver() {
    {
        std::scoped_lock lock{mutex};
        present_thread.request_stop();
    }
    request_cv.notify_all();
    if (present_thread.joinable()) present_thread.join();
}

int VideoOutDriver::Open(const ServiceThreadParams* params) {
    if (main_port.is_open) {
        return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    }
    main_port.is_open = true;
    liverpool->SetVoPort(&main_port);
    return 1;
}

void VideoOutDriver::Close(s32 handle) {
    std::scoped_lock lock{mutex};

    // Mark as closed
    main_port.is_open = false;
    main_port.flip_rate.store(0, std::memory_order_relaxed);
    main_port.prev_index = -1;

    // Clear port information
    std::memset(main_port.buffer_labels.data(), 0, sizeof(main_port.buffer_labels));
    std::memset(main_port.groups.data(), 0, sizeof(main_port.groups));
    std::memset(&main_port.vblank_status, 0, sizeof(main_port.vblank_status));
    main_port.flip_status = FlipStatus{};

    // Re-initialize buffers
    std::memset(main_port.buffer_slots.data(), 0, sizeof(main_port.buffer_slots));
    for (auto& buffer : main_port.buffer_slots) {
        buffer.group_index = -1;
    }

    // Clear events
    for (auto event : main_port.flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.flip_events.clear();
    for (auto event : main_port.vblank_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.vblank_events.clear();
}

VideoOutPort* VideoOutDriver::GetPort(int handle) {
    if (handle != 1) [[unlikely]] {
        return nullptr;
    }
    return &main_port;
}

int VideoOutDriver::RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses,
                                    s32 bufferNum, const BufferAttribute* attribute) {
    const s32 group_index = port->FindFreeGroup();
    if (group_index >= MaxDisplayBufferGroups) {
        return ORBIS_VIDEO_OUT_ERROR_NO_EMPTY_SLOT;
    }

    if (startIndex + bufferNum > MaxDisplayBuffers || startIndex > MaxDisplayBuffers ||
        bufferNum > MaxDisplayBuffers) {
        LOG_ERROR(Lib_VideoOut,
                  "Attempted to register too many buffers startIndex = {}, bufferNum = {}",
                  startIndex, bufferNum);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    const s32 end_index = startIndex + bufferNum;
    if (bufferNum > 0 &&
        std::any_of(port->buffer_slots.begin() + startIndex, port->buffer_slots.begin() + end_index,
                    [](auto& buffer) { return buffer.group_index != -1; })) {
        return ORBIS_VIDEO_OUT_ERROR_SLOT_OCCUPIED;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "startIndex = {}, bufferNum = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             startIndex, bufferNum, GetPixelFormatString(attribute->pixel_format),
             attribute->aspect_ratio, static_cast<u32>(attribute->tiling_mode), attribute->width,
             attribute->height, attribute->pitch_in_pixel, attribute->option);

    auto& group = port->groups[group_index];
    std::memcpy(&group.attrib, attribute, sizeof(BufferAttribute));
    group.is_occupied = true;

    for (u32 i = 0; i < bufferNum; i++) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        port->buffer_slots[startIndex + i] = VideoOutBuffer{
            .group_index = group_index,
            .address_left = address,
            .address_right = 0,
        };

        // Reset flip label also when registering buffer
        port->buffer_labels[startIndex + i] = 0;
        port->SignalVoLabel();

        presenter->RegisterVideoOutSurface(group, address);
        LOG_INFO(Lib_VideoOut, "buffers[{}] = {:#x}", i + startIndex, address);
    }

    return group_index;
}

int VideoOutDriver::UnregisterBuffers(VideoOutPort* port, s32 attributeIndex) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    auto& group = port->groups[attributeIndex];
    group.is_occupied = false;

    for (auto& buffer : port->buffer_slots) {
        if (buffer.group_index != attributeIndex) {
            continue;
        }
        buffer.group_index = -1;
    }

    return ORBIS_OK;
}

int VideoOutDriver::ChangeBufferAttribute(VideoOutPort* port, s32 attributeIndex,
                                          const BufferAttribute* attribute) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "attributeIndex = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             attributeIndex, GetPixelFormatString(attribute->pixel_format), attribute->aspect_ratio,
             static_cast<u32>(attribute->tiling_mode), attribute->width, attribute->height,
             attribute->pitch_in_pixel, attribute->option);

    std::unique_lock lock{port->port_mutex};
    std::memcpy(&port->groups[attributeIndex].attrib, attribute, sizeof(BufferAttribute));
    return 0;
}

// PERF-DIAG-002: flip pacing. Submit-to-flip latency, Present() time, and queued flips at each
// vblank, reported every 2 s.
#ifndef NDEBUG
namespace {
struct FlipStats {
    std::mutex mutex;
    std::chrono::steady_clock::time_point window{};
    u32 flips{}, vblanks{}, idle_vblanks{}, max_queue{}, early_flips{};
    double latency_ms{}, max_latency_ms{}, present_ms{}, max_present_ms{};
};
FlipStats g_flip_stats;

void ReportFlipStats() {
    auto& s = g_flip_stats;
    const auto now = std::chrono::steady_clock::now();
    if (s.window == std::chrono::steady_clock::time_point{}) {
        s.window = now;
        return;
    }
    if (now - s.window < std::chrono::seconds{2}) {
        return;
    }
    LOG_WARNING(Lib_VideoOut,
                "Flips in {:.1f} s: {} flips ({} between vblanks), {} vblanks ({} with no flip ready), max queued {}, "
                "submit-to-flip avg {:.2f} ms max {:.2f} ms, Present avg {:.2f} ms max {:.2f} ms",
                std::chrono::duration<double>(now - s.window).count(), s.flips, s.early_flips, s.vblanks,
                s.idle_vblanks, s.max_queue, s.flips ? s.latency_ms / s.flips : 0.0,
                s.max_latency_ms, s.flips ? s.present_ms / s.flips : 0.0, s.max_present_ms);
    s.flips = s.vblanks = s.idle_vblanks = s.max_queue = s.early_flips = 0;
    s.latency_ms = s.max_latency_ms = s.present_ms = s.max_present_ms = 0;
    s.window = now;
}
} // namespace
#endif

void VideoOutDriver::Flip(const Request& req) {
#ifndef NDEBUG
    Common::FrameQueueTrace::Row trace{};
    trace.ticket = req.trace;
    if (trace.ticket.generation) {
        trace.flip_begin = Common::FrameQueueTrace::Now(trace.ticket);
        trace.ready_semaphore =
            reinterpret_cast<std::uintptr_t>(static_cast<VkSemaphore>(req.frame->ready_semaphore));
        trace.ready_tick = req.frame->ready_tick;
        trace.frame_id = req.frame->id;
    }
    Common::FrameQueueTrace::Scope trace_scope{trace.ticket.generation ? &trace : nullptr};
#endif
    // Update HDR status before presenting.
    presenter->SetHDR(req.port->is_hdr);

    // Present the frame.
#ifndef NDEBUG
    const auto present_start = std::chrono::steady_clock::now();
    Common::Nvtx::Mark("flip"); // DIAG-056
#endif
    {
        Common::Nvtx::Scope nvtx{"present"};
        presenter->Present(req.frame, false, true, req.anti_lag_frame);
    }
    #ifndef NDEBUG
    {
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lk{g_flip_stats.mutex};
        auto& st = g_flip_stats;
        const double present_ms =
            std::chrono::duration<double, std::milli>(now - present_start).count();
        const double latency_ms =
            std::chrono::duration<double, std::milli>(now - req.submitted).count();
        ++st.flips;
        st.present_ms += present_ms;
        st.max_present_ms = std::max(st.max_present_ms, present_ms);
        st.latency_ms += latency_ms;
        st.max_latency_ms = std::max(st.max_latency_ms, latency_ms);
    }

    if (trace.ticket.generation) {
        auto& work = Common::GetWorkCounters();
        trace.draws = work.draws.load(std::memory_order_relaxed);
        trace.dispatches = work.dispatches.load(std::memory_order_relaxed);
        trace.shaders = work.shaders_compiled.load(std::memory_order_relaxed);
        trace.pipelines = work.pipelines_compiled.load(std::memory_order_relaxed);
        trace.uploads = work.uploads.load(std::memory_order_relaxed);
        trace.upload_bytes = work.upload_bytes.load(std::memory_order_relaxed);
        trace.protects = work.protects.load(std::memory_order_relaxed);
        trace.protect_bytes = work.protect_bytes.load(std::memory_order_relaxed);
        Common::FrameQueueTrace::Complete(trace);
    }
#endif
    // Update flip status.
    auto* port = req.port;
    {
        std::unique_lock lock{port->port_mutex};
        auto& flip_status = port->flip_status;
        flip_status.count++;
        flip_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
        flip_status.tsc = Libraries::Kernel::sceKernelReadTsc();
        flip_status.flip_arg = req.flip_arg;
        flip_status.current_buffer = req.index;
        if (req.eop) {
            --flip_status.gc_queue_num;
        }
        --flip_status.flip_pending_num;
    }

    // Trigger flip events for the port.
    for (auto event : port->flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                        (req.flip_arg << 16)));
        }
    }

    // Reset prev flip label
    if (port->prev_index != -1) {
        port->buffer_labels[port->prev_index] = 0;
        port->SignalVoLabel();
    }
    // save to prev buf index
    port->prev_index = req.index;
}

void VideoOutDriver::DrawBlankFrame() {
    const auto empty_frame = presenter->PrepareBlankFrame(true);
    presenter->Present(empty_frame, false, false);
}

bool VideoOutDriver::DrawLastFrame() {
    const auto frame = presenter->PrepareLastFrame();
    if (frame != nullptr) {
        presenter->Present(frame, true);
        return true;
    }
    return false;
}

u64 VideoOutDriver::AntiLagInput() {
    return presenter->AntiLagInput();
}

bool VideoOutDriver::SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg,
                                bool is_eop, u64 anti_lag_frame) {
    auto trace = Common::FrameQueueTrace::Start();
    RyDiag::Mark(RyDiag::Zone::GameFlip, u64(index));
    {
        std::unique_lock lock{port->port_mutex};
        if (index != -1 && port->flip_status.flip_pending_num > 16) {
            LOG_ERROR(Lib_VideoOut, "Flip queue is full");
            return false;
        }

        if (is_eop) {
            ++port->flip_status.gc_queue_num;
        }
        ++port->flip_status.flip_pending_num; // integral GPU and CPU pending flips counter
        port->flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
    }
    if (!is_eop) anti_lag_frame = presenter->AntiLagInput();

    if (!is_eop) {
        // Non EOP flips can arrive from any thread so ask GPU thread to perform them
        liverpool->SendCommand(
            [=, this]() { SubmitFlipInternal(port, index, flip_arg, is_eop, trace, anti_lag_frame); });
    } else {
        SubmitFlipInternal(port, index, flip_arg, is_eop, trace, anti_lag_frame);
    }

    return true;
}

void VideoOutDriver::SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop,
                                        Common::FrameQueueTrace::Ticket trace, u64 anti_lag_frame) {
#ifndef NDEBUG
    if (trace.generation)
        Common::FrameQueueTrace::Prepare(trace);
#endif
    Vulkan::Frame* frame;
    if (index == -1) {
        frame = presenter->PrepareBlankFrame(false);
    } else {
        const auto& buffer = port->buffer_slots[index];
        ASSERT_MSG(buffer.group_index >= 0, "Trying to flip an unregistered buffer!");
        const auto& group = port->groups[buffer.group_index];
        frame = presenter->PrepareFrame(group, buffer.address_left);
    }

    {
        std::scoped_lock lock{mutex};
#ifndef NDEBUG
        if (trace.generation)
            Common::FrameQueueTrace::Enqueue(trace);
#endif
        requests.push({
            .frame = frame,
            .port = port,
            .flip_arg = flip_arg,
            .index = index,
            .eop = is_eop,
            .anti_lag_frame = anti_lag_frame,
            .trace = trace,
#ifndef NDEBUG
            .submitted = std::chrono::steady_clock::now(),
#endif
        });
    }
    request_cv.notify_one();
}

void VideoOutDriver::PresentThread(std::stop_token token) {
    const std::chrono::nanoseconds vblank_period(1000000000 /
                                                 EmulatorSettings.GetVblankFrequency());
    // FAST-FLIP (ryGT): a frame finished between two vblanks is flipped at once instead of at
    // the next vblank, so a 25 ms frame shows after 25 ms rather than 33 ms (GT Sport's races
    // otherwise snap to 30 fps). Enqueue wakes the thread between four scheduled ticks per
    // vblank; the game's vblank events and status still come once per vblank.
    // The native renderer defaults to this path without frame generation; an explicit
    // GPU.fast_flips setting or RYGT_FAST_FLIPS overrides that default. The elapsed-time
    // limit still honors the guest flip rate and the configured FPS lock.
    static const u32 ticks_per_vblank = [] {
        std::optional<bool> configured;
        bool native = false;
#ifdef RYTY_GPU
        configured = Ryty::ConfigBool("fast_flips");
        native = Vulkan::NativeModeRequested();
#endif
        const bool fast = FastFlipsEnabled(std::getenv("RYGT_FAST_FLIPS"), configured, native,
                                           EmulatorSettings.IsFrameGenEnabled() ||
                                               EmulatorSettings.IsDlssgEnabled());
        if (fast) {
            LOG_INFO(Lib_VideoOut, "FAST-FLIP: frames flip when ready, between vblanks");
        }
        return fast ? 4u : 1u;
    }();
    const auto tick_period = vblank_period / ticks_per_vblank;
    u32 tick = 0;

    Common::SetCurrentThreadName("shadGT:PresentThread");
    Common::SetCurrentThreadRealtime(tick_period);

    using Clock = std::chrono::steady_clock;
    auto next_tick = Clock::now();
    auto last_flip_time = Clock::time_point{};
    const auto current_flip_rate = [&] {
        static const u32 fps_lock = [] {
            const char* env = std::getenv("SHADGT_FPS_LOCK");
            const u32 fps = env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 0;
            if (fps != 0) {
                LOG_WARNING(Lib_VideoOut, "PERF-075: flips locked to at most {} per second", fps);
            }
            return fps;
        }();
        return LockedFlipRate(main_port.flip_rate.load(std::memory_order_relaxed), fps_lock,
                              EmulatorSettings.GetVblankFrequency());
    };
    const auto early_allowed = [&] {
        return ticks_per_vblank > 1 && !DebugState.IsGuestThreadsPaused() &&
               !Common::GuestClock::IsPaused();
    };
    const auto flip_allowed_at = [&] {
        return last_flip_time + FastFlipInterval(vblank_period, current_flip_rate());
    };

    const auto receive_request = [this] -> Request {
        std::scoped_lock lk{mutex};
        if (!requests.empty()) {
            auto request = requests.front();
            requests.pop();
#ifndef NDEBUG
            if (request.trace.generation)
                Common::FrameQueueTrace::Dequeue(request.trace);
#endif
            return request;
        }
        return {};
    };

    while (!token.stop_requested()) {
        auto now = Clock::now();
        if (now < next_tick) {
            if (early_allowed()) {
                std::unique_lock lock{mutex};
                if (requests.empty()) {
                    request_cv.wait_until(lock, next_tick,
                                          [&] { return token.stop_requested() || !requests.empty(); });
                } else if (const auto allowed = flip_allowed_at(); Clock::now() < allowed) {
                    request_cv.wait_until(lock, std::min(next_tick, allowed));
                }
                lock.unlock();
                if (token.stop_requested()) {
                    break;
                }
                now = Clock::now();
                if (now < next_tick && early_allowed()) {
                    if (now >= flip_allowed_at()) {
                        if (const auto request = receive_request()) {
#ifndef NDEBUG
                            {
                                std::scoped_lock stats_lock{g_flip_stats.mutex};
                                ++g_flip_stats.early_flips;
                            }
#endif
                            Flip(request);
                            last_flip_time = Clock::now();
                            FRAME_END;
                        }
                    }
                    continue; // early wakes do not advance the vblank deadline
                }
            }
            now = Clock::now();
            if (now < next_tick) {
                Common::AccurateSleep(next_tick - now, nullptr, false);
            }
        }
        if (token.stop_requested()) {
            break;
        }
        const bool overdue = Clock::now() > next_tick;
        next_tick += tick_period;

        if (DebugState.IsGuestThreadsPaused()) {
            DrawLastFrame();
            continue;
        }
        if (Common::GuestClock::IsPaused()) {
            // FIX-044: no vblank passes while the guest clocks are held for a shader or
            // pipeline compile, as no time passes for the game. PERF-100: the last frame is
            // shown again, with the compile notice drawn over it (PERF-102: or the splash
            // screen, before the game's first frame).
            if (overdue) {
                // Before the game's first frame a blank frame, which shows the splash screen
                // and the precompile bar; after it the last frame (skipped while it is taken).
                // DIAG-126 (SHADGT_NO_LAST_FRAME=1): compile pauses after the first frame keep
                // what is on screen instead of showing the last frame again.
#ifndef NDEBUG
                static const bool no_last_frame = [] {
                    const char* env = std::getenv("SHADGT_NO_LAST_FRAME");
                    return env && env[0] == '1';
                }();
#else
                constexpr bool no_last_frame = false;
#endif
                if (!main_port.is_open || !presenter->HasLastFrame()) {
                    DrawBlankFrame();
                } else if (!no_last_frame) {
                    DrawLastFrame();
                }
            }
            continue;
        }

        // Check if it's time to take a request.
        auto& vblank_status = main_port.vblank_status;
        const int flip_rate = current_flip_rate();
        const bool vblank_tick = tick++ % ticks_per_vblank == 0;
        if (!vblank_tick) {
            // FAST-FLIP: between vblanks, only a ready frame is flipped.
            if (Clock::now() >= flip_allowed_at()) {
                if (const auto request = receive_request()) {
#ifndef NDEBUG
                    {
                        std::scoped_lock lk{g_flip_stats.mutex};
                        ++g_flip_stats.early_flips;
                    }
#endif
                    Flip(request);
                    last_flip_time = Clock::now();
                    FRAME_END;
                }
            }
            continue;
        }
        if (vblank_status.count % (flip_rate + 1) == 0) {
#ifndef NDEBUG
            {
                std::scoped_lock lk{mutex, g_flip_stats.mutex};
                ++g_flip_stats.vblanks;
                g_flip_stats.max_queue =
                    std::max<u32>(g_flip_stats.max_queue, static_cast<u32>(requests.size()));
                if (requests.empty()) {
                    ++g_flip_stats.idle_vblanks;
                }
                ReportFlipStats();
            }
#endif
            const auto request = ticks_per_vblank == 1 || Clock::now() >= flip_allowed_at()
                                     ? receive_request() : Request{};
            if (!request) {
                if (overdue) { // Dont draw too fast
                    if (!main_port.is_open) {
                        DrawBlankFrame();
                    } else if (ImGui::Core::MustKeepDrawing()) {
                        DrawLastFrame();
                    }
                }
            } else {
                Flip(request);
                last_flip_time = Clock::now();
                FRAME_END;
            }
        }

        {
            // Needs lock here as can be concurrently read by `sceVideoOutGetVblankStatus`
            std::scoped_lock lock{main_port.vo_mutex};

            // Trigger flip events for the port
            for (auto event : main_port.vblank_events) {
                auto equeue = Kernel::GetEqueue(event);
                if (equeue != nullptr) {
                    equeue->TriggerEvent(
                        static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                        Kernel::OrbisKernelEvent::Filter::VideoOut,
                        reinterpret_cast<void*>(
                            static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) |
                            (vblank_status.count << 16)));
                }
            }

            // Update vblank status
            vblank_status.count++;
            vblank_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
            vblank_status.tsc = Libraries::Kernel::sceKernelReadTsc();
            main_port.vblank_cv.notify_all();
        }

    }
}

} // namespace Libraries::VideoOut
