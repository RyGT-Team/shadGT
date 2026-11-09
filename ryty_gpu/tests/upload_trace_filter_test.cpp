#include "../upload_trace_filter.h"
#include <cassert>
int main() {
    assert(ImageLife::UploadKind("workload_summary"));
    assert(ImageLife::UploadKind("texture_entry_stale"));
    assert(ImageLife::UploadKind("texture_cpu_guarded_copy"));
    assert(ImageLife::UploadKind("texture_source_result"));
    assert(ImageLife::UploadKind("texture_registry_fallback"));
    assert(!ImageLife::UploadKind("origin_write_fault"));
    assert(!ImageLife::UploadKind(nullptr));
}
