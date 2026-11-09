#pragma once
#include <cstring>
namespace ImageLife {
inline bool PresentationKind(const char* kind) {
    constexpr const char* allowed[] = {"present", "frame_prepared", "presentation_prepare_frame",
        "presentation_free_queue", "presentation_frame_fence", "presentation_swapchain_acquire",
        "presentation_queue_present", "presentation_wait_request", "presentation_wait_flip_rate",
        "presentation_tick_sleep"};
    for (const char* name : allowed) if (std::strcmp(kind, name) == 0) return true;
    return false;
}
}
