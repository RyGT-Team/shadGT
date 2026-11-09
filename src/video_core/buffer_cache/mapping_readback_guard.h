// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/buffer_cache/range_set.h"

namespace VideoCore {
// All calls require the same lock that protects readback validity and completion.
class MappingReadbackGuard {
public:
    void BeginUnmap(VAddr address, u64 size) { unavailable.Add(address, size); }
    bool MayComplete(VAddr address, u64 size) const { return !unavailable.Intersects(address, size); }
    template<class Invalidate>
    void PublishMap(VAddr address, u64 size, Invalidate invalidate_old) {
        invalidate_old(); // gap imports/readbacks cannot become valid on the new mapping
        unavailable.Subtract(address, size);
    }
private:
    RangeSet unavailable;
};
}
