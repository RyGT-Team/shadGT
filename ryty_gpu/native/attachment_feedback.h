// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Native {
struct DepthFeedbackDecision {
    bool active;
    bool read_only;
    bool unsupported;
};

// PERF-SGT-150: whether a draw with the stencil test on can change the stencil: a non-zero write
// mask and an op other than Keep (op 0) on a face that is drawn. Without backface_enable the
// front settings apply to both faces. A draw that can't is a reader of its depth-stencil target,
// so sampling that target takes the read-only layout instead of a feedback loop or, without the
// feedback-loop extensions (AMD's Windows driver), a copy of the whole target before each draw.
constexpr bool StencilMayWrite(bool stencil_test, bool backface_enable,
                               unsigned front_fail, unsigned front_zpass, unsigned front_zfail,
                               unsigned back_fail, unsigned back_zpass, unsigned back_zfail,
                               unsigned front_write_mask, unsigned back_write_mask) {
    if (!stencil_test) return false;
    const bool front = front_write_mask != 0 &&
                       (front_fail != 0 || front_zpass != 0 || front_zfail != 0);
    if (!backface_enable) return front;
    const bool back = back_write_mask != 0 &&
                      (back_fail != 0 || back_zpass != 0 || back_zfail != 0);
    return front || back;
}

constexpr DepthFeedbackDecision DecideDepthFeedback(bool sampled_attachment,
                                                     bool writes_attachment,
                                                     bool extension_and_features) {
    return {sampled_attachment && writes_attachment && extension_and_features,
            sampled_attachment && !writes_attachment,
            sampled_attachment && writes_attachment && !extension_and_features};
}
constexpr bool BlocksParallelFeedback(bool transitioned_attachment, bool snapshot) {
    return transitioned_attachment || snapshot;
}
} // namespace Native
