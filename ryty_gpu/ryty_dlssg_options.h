// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>

#include "ryty_gpu/streamline/sl_result.h"

namespace Vulkan::RytyDlssG {

constexpr bool OptionsAccepted(sl::Result result) {
    return result == sl::Result::eOk || result == sl::Result::eWarnOutOfVRAM;
}

class GenerationOptions {
public:
    using Clock = std::chrono::steady_clock;

    struct Update {
        sl::Result result = sl::Result::eOk;
        bool attempted{};
        bool applied{};
        bool enabled{};
    };

    template <typename SetOptions>
    Update Apply(Clock::time_point now, bool requested, SetOptions&& set_options) {
        if (known && enabled == requested && !stop_pending) {
            return {sl::Result::eOk, false, true, enabled};
        }
        if (requested && !stop_pending && now < retry_after) {
            return {};
        }
        const bool target = requested && !stop_pending;
        const auto result = set_options(target);
        if (OptionsAccepted(result)) {
            known = true;
            enabled = target;
            stop_pending = false;
            return {result, true, requested == target, target};
        }
        known = false;
        stop_pending = target;
        if (target) {
            retry_after = now + std::chrono::seconds{2};
        }
        return {result, true, false, target};
    }

private:
    Clock::time_point retry_after{};
    bool known{};
    bool enabled{};
    bool stop_pending{};
};

} // namespace Vulkan::RytyDlssG
