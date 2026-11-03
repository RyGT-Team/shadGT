// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/frame_queue_trace.h"
#include "common/debug_environment.h"
#include <cstdlib>
#include <filesystem>
#include <type_traits>

#ifndef NDEBUG
#error This test must exercise the production Release configuration.
#endif

static_assert(std::is_empty_v<Common::FrameQueueTrace::Ticket>);
static_assert(std::is_empty_v<Common::FrameQueueTrace::Row>);
static_assert(std::is_empty_v<Common::FrameQueueTrace::Scope>);
static_assert(Common::DebugEnv("RYGT_FRAME_QUEUE_TRACE") == nullptr);

static bool SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    const std::filesystem::path base{argv[1]};
    const auto output = base.string() + ".csv";
    const auto control = base.string() + ".control";
    const auto status = base.string() + ".status";
    if (std::filesystem::exists(output) || std::filesystem::exists(status))
        return 2;
    if (!SetEnvironment("RYGT_FRAME_QUEUE_TRACE", "1") ||
        !SetEnvironment("RYGT_FRAME_QUEUE_TRACE_OUTPUT", output.c_str()) ||
        !SetEnvironment("RYGT_FRAME_QUEUE_TRACE_CONTROL", control.c_str()) ||
        !SetEnvironment("RYGT_FRAME_QUEUE_TRACE_STATUS", status.c_str()))
        return 3;
    namespace Trace = Common::FrameQueueTrace;
    Trace::Initialize();
    auto ticket = Trace::Start();
    Trace::Prepare(ticket);
    Trace::Enqueue(ticket);
    Trace::Dequeue(ticket);
    Trace::Row row;
    {
        Trace::Scope scope{&row};
        if (Trace::current != nullptr || Trace::Valid(ticket) || Trace::Now(ticket) != 0)
            return 4;
        Trace::Complete(row);
    }
    return std::filesystem::exists(output) || std::filesystem::exists(status) ? 5 : 0;
}
