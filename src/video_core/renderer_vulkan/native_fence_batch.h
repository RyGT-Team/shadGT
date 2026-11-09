// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace Vulkan {
inline std::int64_t NativeFenceBatchNs(std::string_view text) {
    constexpr std::int64_t Default = 2'000'000;
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == text.npos) return Default;
    text.remove_prefix(first);
    text = text.substr(0, text.find_last_not_of(" \t\r\n") + 1);
    double ms{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), ms);
    // Keep malformed settings from turning a batching interval into an indefinite wait.
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(ms) ||
        ms < 0 || ms > 1000) return Default;
    return static_cast<std::int64_t>(ms * 1'000'000.0);
}

inline bool SubmitNativeFence(bool pipelined, bool flush, bool hold_flush, bool readbacks, bool address_pending,
                               bool direct_hold, std::int64_t now, std::int64_t last_submit,
                               std::int64_t interval) {
    if (hold_flush || readbacks || address_pending || !direct_hold) return true;
    if (!flush) return false;
    if (!pipelined) return true; // no recorder-idle progress hook in this mode
    return interval <= 0 || last_submit <= 0 || now < last_submit || now - last_submit >= interval;
}
} // namespace Vulkan
