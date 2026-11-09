#pragma once
#include "ryty_gpu/image_lifecycle.h"
#include <array>
#include <atomic>
#include <cstdint>
namespace WorkloadTrace {
enum Field : unsigned { Draws, Dispatches, ImageUploadRequestedBytes, HashBytes, Count };
inline std::array<std::atomic<std::uint64_t>, Count> counters{};
inline void Add(Field field, std::uint64_t value=1) {
    if (ImageLife::Enabled("workload_summary")) counters[field].fetch_add(value,std::memory_order_relaxed);
}
inline std::array<std::uint64_t,Count> Snapshot() {
    std::array<std::uint64_t,Count> values{};
    for(unsigned i=0;i<Count;++i) values[i]=counters[i].load(std::memory_order_relaxed);
    return values;
}
}
