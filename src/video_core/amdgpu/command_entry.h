#pragma once
#include <cstdint>
#include <utility>
#include "common/unique_function.h"
namespace AmdGpu {
struct CommandEntry {
    Common::UniqueFunction<void> callback;
    std::uint64_t trace_id{};
    template<class F> explicit CommandEntry(F&& f, std::uint64_t id=0)
        : callback(std::forward<F>(f)), trace_id(id) {}
};
}
