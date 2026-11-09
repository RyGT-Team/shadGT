// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
namespace Vulkan {
inline bool DelayFailedTranslationRetry(std::chrono::steady_clock::time_point deadline,
    std::chrono::steady_clock::time_point now, bool speculative, bool same_inputs) {
    return speculative && same_inputs && now < deadline;
}
}
