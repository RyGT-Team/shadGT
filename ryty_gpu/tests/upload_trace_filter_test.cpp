#include "../upload_trace_filter.h"
#include <cassert>
int main() {
    assert(ImageLife::UploadKind("workload_summary"));
    assert(ImageLife::UploadKind("texture_entry_stale"));
    assert(ImageLife::UploadKind("texture_cpu_guarded_copy"));
    assert(ImageLife::UploadKind("texture_source_result"));
    assert(ImageLife::UploadKind("texture_registry_fallback"));
    assert(ImageLife::UploadKind("upload_take"));
    assert(ImageLife::UploadKind("upload_free_before_reclaim"));
    assert(ImageLife::UploadKind("upload_free_after_reclaim"));
    assert(ImageLife::UploadKind("upload_retiring"));
    assert(ImageLife::UploadKind("upload_create_buffer"));
    assert(ImageLife::UploadKind("upload_allocation_info"));
    assert(ImageLife::UploadKind("upload_heap_info"));
    assert(ImageLife::UploadKind("upload_block_info"));
    assert(!ImageLife::UploadKind("origin_write_fault"));
    assert(!ImageLife::UploadKind(nullptr));
}
