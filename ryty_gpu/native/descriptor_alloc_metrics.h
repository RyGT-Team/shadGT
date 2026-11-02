#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
namespace Native {
enum class DescriptorResult : std::uint8_t { Success, Capacity, Other };
struct DescriptorTiming { std::uint64_t samples{}, cpu_samples{}, wall_ns{}, wall_max_ns{}, cpu_100ns{}, cpu_max_100ns{}; };
struct DescriptorAllocMetrics {
    // [single/batch][attempt 0/1/2][success/capacity/other]
    std::uint64_t results[2][3][3]{};
    DescriptorTiming timing[2][3]{};
    // [graphics/compute][non-push reserve misses 0..5, overflow]
    std::uint64_t misses[2][7]{};
    std::uint64_t cache_hits[2]{}, reserve_hits[2]{}, pool_initial{}, pool_rotate{}, sequence{};
    bool NextSample() { return (++sequence & 127u) == 0; }
    void Draw(unsigned kind, unsigned miss_count) { ++misses[std::min(kind,1u)][std::min(miss_count,6u)]; }
    void Attempt(unsigned batch, unsigned attempt, DescriptorResult result, bool sample,
                 std::uint64_t wall_ns, bool cpu_valid, std::uint64_t cpu_ticks) {
        batch=std::min(batch,1u); attempt=std::min(attempt,2u);
        ++results[batch][attempt][static_cast<unsigned>(result)];
        if (!sample) return;
        auto& t=timing[batch][attempt]; ++t.samples; t.wall_ns+=wall_ns; t.wall_max_ns=std::max(t.wall_max_ns,wall_ns);
        if(cpu_valid) {++t.cpu_samples; t.cpu_100ns+=cpu_ticks; t.cpu_max_100ns=std::max(t.cpu_max_100ns,cpu_ticks);}
    }
};
}
