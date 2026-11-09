#pragma once
#include <cstring>
namespace ImageLife {
inline bool UploadKind(const char* kind) {
    if (!kind) return false;
    constexpr const char* allowed[] = {"workload_summary", "present", "frame_prepared",
        "texture_entry_create", "texture_entry_evict", "texture_entry_stale", "texture_entry_upload",
        "texture_source_select", "texture_source_entry", "texture_policy_readable",
        "texture_ensure_imported", "texture_source_result", "texture_registry_fallback", "texture_content_probe",
        "texture_alias_probe", "texture_alias_probe_detail", "upload_take",
        "upload_free_before_reclaim", "upload_free_after_reclaim", "upload_retiring",
        "upload_create_buffer", "upload_allocation_info", "upload_heap_info", "upload_block_info"};
    for (const char* name : allowed) if (!std::strcmp(kind,name)) return true;
    return !std::strncmp(kind,"texture_cpu_",12);
}
}
