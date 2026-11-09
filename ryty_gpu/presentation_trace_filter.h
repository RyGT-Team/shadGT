#pragma once
#include <cstring>
namespace ImageLife {
inline bool PresentationSlowKind(const char* kind) {
    if (!kind) return false;
    constexpr const char* allowed[] = {"producer_pipeline_lock", "producer_job_walk", "producer_pipeline_lookup",
        "producer_draw_prepare", "fault_early_fence", "fault_guard", "fault_read_handler",
        "unmap_native", "unmap_direct", "unmap_buffer", "unmap_gpu_sync", "unmap_texture",
        "unmap_sparse_release", "unmap_sparse_finish"};
    for (const char* name : allowed) if (std::strcmp(kind, name) == 0) return true;
    return false;
}
inline bool PresentationKind(const char* kind) {
    constexpr const char* allowed[] = {"present", "frame_prepared", "presentation_prepare_frame",
        "presentation_free_queue", "presentation_frame_fence", "presentation_swapchain_acquire",
        "presentation_queue_present", "presentation_wait_request", "presentation_wait_flip_rate",
        "presentation_tick_sleep", "upload_take", "upload_free_before_reclaim",
        "upload_free_after_reclaim", "upload_retiring", "upload_create_buffer",
        "prepared_summary_0", "prepared_summary_1", "prepared_summary_2", "prepared_summary_3",
        "unmap_memory"};
    for (const char* name : allowed) if (std::strcmp(kind, name) == 0) return true;
    return false;
}
}
