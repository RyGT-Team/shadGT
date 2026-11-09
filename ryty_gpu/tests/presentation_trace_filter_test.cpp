#include "../presentation_trace_filter.h"
#include <cassert>
int main() {
    assert(ImageLife::PresentationKind("present"));
    assert(ImageLife::PresentationKind("frame_prepared"));
    assert(ImageLife::PresentationKind("presentation_wait_flip_rate"));
    assert(ImageLife::PresentationKind("presentation_swapchain_acquire"));
    assert(!ImageLife::PresentationKind("origin_write_fault"));
    assert(!ImageLife::PresentationKind("texture_entry_stale"));
    assert(!ImageLife::PresentationKind("presentation_unknown"));
}
