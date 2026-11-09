// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Native {
struct DepthFeedbackDecision {
    bool active;
    bool read_only;
    bool unsupported;
};

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
