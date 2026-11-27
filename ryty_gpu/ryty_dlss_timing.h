// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdlib>
#include <string_view>

#include <SDL3/SDL_timer.h>
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

struct DlssTiming {
    const Instance& instance;
    Scheduler& scheduler;

    // Restrict timestamps to the vendor evaluation. SDK setup, HUD and presentation stay outside.
    template <typename Evaluate, typename Result>
    static Result Measure(DlssTiming* timer, vk::CommandBuffer cmd, vk::Extent2D input,
                          vk::Extent2D output, const char* kind, const char* preset,
                          Evaluate evaluate, Result success) {
        static const bool enabled = [] {
            const char* value = std::getenv("RYGT_PROFILE_DLSS");
            return value && std::string_view(value) == "1";
        }();
        static thread_local unsigned frame = 0;
        if (!enabled || !timer || frame++ % 16 != 0) return evaluate();

        auto physical = timer->instance.GetPhysicalDevice();
        auto device = timer->instance.GetDevice();
        const auto bits = physical.getQueueFamilyProperties()[timer->instance.GetGraphicsQueueFamilyIndex()].timestampValidBits;
        if (!bits) return evaluate();
        auto query = device.createQueryPoolUnique({.queryType = vk::QueryType::eTimestamp, .queryCount = 2});
        if (query.result != vk::Result::eSuccess) return evaluate();
        const auto pool = *query.value;
        const float period = physical.getProperties().limits.timestampPeriod;
        // SDL's performance counter is QPC on Windows, the same timebase as PresentMon.
        const auto qpc = SDL_GetPerformanceCounter();
        cmd.resetQueryPool(pool, 0, 2);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, 0);
        const auto result = evaluate();
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, 1);
        timer->scheduler.DeferOperation([owner = std::move(query.value), device, bits, period,
                                          qpc, input, output, kind, preset, ok = result == success] {
            std::array<u64, 2> ts{};
            if (device.getQueryPoolResults(*owner, 0, 2, sizeof(ts), ts.data(), sizeof(u64),
                                           vk::QueryResultFlagBits::e64) != vk::Result::eSuccess) return;
            const auto mask = ~u64{0} >> (64 - bits);
            const double ms = double((ts[1] - ts[0]) & mask) * period / 1000000.;
            LOG_INFO(Render_Vulkan, "DLSS GPU sample qpc={} kind={} preset={} input={}x{} output={}x{} ms={:.4f}",
                     qpc, ok ? kind : "failed", preset, input.width, input.height,
                     output.width, output.height, ms);
        });
        return result;
    }
};

} // namespace Vulkan
