#pragma once
#include <array>
#include <atomic>
#include <cstdint>
namespace PreparedTrace {
enum Field : unsigned { Missing, Accepted, Pipeline, Stage, EarlyUnmap, Image, EarlyBuffer,
    LateUnmap, LateBuffer, VertexUnmap, VertexBuffer, RegionRebuild, Incomplete,
    VertexFallback, NonflatBytes, FlatHitBytes, FlatMissBytes, NonacceptedBytes, NotEligible, Count };
struct Outcome {
    bool enabled{}; Field result{Incomplete}; bool vertex_fallback{};
    std::uint64_t nonflat{}, flat_hit{}, flat_miss{};
};
struct Aggregate {
    std::array<std::atomic<std::uint64_t>,Count> values{};
    void Publish(const Outcome& o) noexcept {
        if (!o.enabled) return;
        values[o.result].fetch_add(1,std::memory_order_relaxed);
        if(o.vertex_fallback) values[VertexFallback].fetch_add(1,std::memory_order_relaxed);
        values[NonflatBytes].fetch_add(o.nonflat,std::memory_order_relaxed);
        values[FlatHitBytes].fetch_add(o.flat_hit,std::memory_order_relaxed);
        values[FlatMissBytes].fetch_add(o.flat_miss,std::memory_order_relaxed);
        if(o.result != Accepted) values[NonacceptedBytes].fetch_add(o.nonflat+o.flat_miss,std::memory_order_relaxed);
    }
    std::array<std::uint64_t,Count> Drain() noexcept {
        std::array<std::uint64_t,Count> out{};
        for(unsigned i=0;i<Count;++i) out[i]=values[i].exchange(0,std::memory_order_relaxed);
        return out;
    }
};
}
