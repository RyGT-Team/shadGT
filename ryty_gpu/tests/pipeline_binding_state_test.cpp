#include "video_core/renderer_vulkan/pipeline_binding_state.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <stdexcept>

int main() {
    using Cache = Vulkan::PipelineBindingState<unsigned long long, true>;
    Cache alternating;
    unsigned binds = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        alternating.Graphics(1, [&] { ++binds; });
        alternating.Compute(2, false, [&] { ++binds; });
    }
    assert(binds == 2 && alternating.reused[0] == 999 && alternating.reused[1] == 999);
    // Multiple streams, helper clobbers, new lists, repeated/equal-valued handles,
    // Intel forced compute bindings, and counter resets against a driver model.
    std::array<Cache, 3> caches;
    std::array<std::array<unsigned long long, 2>, 3> driver{};
    unsigned long long rng = 19;
    unsigned emitted = 0, reused = 0;
    for (unsigned i = 0; i < 1000000; ++i) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        unsigned stream = (rng >> 8) % caches.size();
        auto& cache = caches[stream];
        auto& actual = driver[stream];
        if (rng % 11 == 0) {
            actual = {99, 98}; // unknown helper state must not survive invalidation
            cache.Invalidate();
        }
        if (rng % 17 == 0) { actual = {}; cache.Invalidate(); } // new command list
        if (rng % 97 == 0) cache.ResetCounts(); // logging must not invalidate bindings
        unsigned point = rng & 1;
        unsigned long long pipeline = (rng >> 16) % 8 + 1;
        bool force = point && (rng % 7 == 0);
        const bool expected = force || actual[point] != pipeline;
        const auto other = actual[1 - point];
        unsigned calls = 0;
        auto emit = [&] { ++calls; actual[point] = pipeline; };
        if (point) cache.Compute(pipeline, force, emit);
        else cache.Graphics(pipeline, emit);
        assert(calls == unsigned(expected));
        assert(actual[point] == pipeline && actual[1 - point] == other);
        emitted += calls;
        reused += !calls;
    }
    Cache failed;
    try {
        failed.Graphics(7, [] { throw std::runtime_error("failed recording"); });
        assert(false);
    } catch (const std::runtime_error&) {}
    assert(failed.handles[0] == 0 && failed.emitted[0] == 0);
    puts("PASS: 1,000,000 mixed-stream binding/invalidation/force cases; alternating binds 2000 -> 2");
    std::printf("Mixed workload emitted=%u reused=%u\n", emitted, reused);
}
