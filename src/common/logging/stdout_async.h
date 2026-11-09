// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <memory>
#include <utility>
#include <spdlog/sinks/async_sink.h>

namespace Common::Log {
// A diagnostic consumer must not back-pressure rendering. Old diagnostic records
// may be replaced if the bounded queue fills; fatal reports also go to stderr.
inline std::shared_ptr<spdlog::sinks::async_sink> MakeAsyncStdoutSink(
    std::shared_ptr<spdlog::sinks::sink> console, std::function<void()> start = {}) {
    return std::make_shared<spdlog::sinks::async_sink>(
        spdlog::sinks::async_sink::config{
            .queue_size = 8192,
            .policy = spdlog::sinks::async_sink::overflow_policy::overrun_oldest,
            .sinks = {std::move(console)},
            .on_thread_start = std::move(start)});
}
}
