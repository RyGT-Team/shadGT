// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include "gpu-timing-flag-under-test.inc"
int main() {
    assert(!GpuTimingRequested(nullptr));assert(!GpuTimingRequested(""));assert(!GpuTimingRequested("0"));
    assert(GpuTimingRequested("1"));assert(!GpuTimingRequested("11"));assert(!GpuTimingRequested("true"));
}
