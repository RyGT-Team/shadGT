#pragma once
#include <cstdint>
#include <limits>
#include <span>

namespace Native::Parallel {
// A zero-address sharp can only authorize its uniquely corresponding committed
// sampled-image write. The caller validates the actual descriptor slice and
// canonical null, and invokes this on the already installed prepared stages.
template<class Stage, class SetOf, class StartOf>
bool AbsentSampledImage(std::span<const Stage* const> stages, std::uint32_t set,
                        std::uint32_t binding, std::uint32_t array_element,
                        std::uint32_t count, SetOf set_of, StartOf start_of) {
    if (array_element != 0 || !count) return false;
    constexpr auto max = std::numeric_limits<std::uint32_t>::max();
    unsigned matches = 0;
    bool absent = false;
    for (const auto* stage : stages) {
        if (!stage || set_of(*stage) != set) continue;
        const auto start = start_of(*stage);
        if (start > max) return false;
        std::uint64_t current = start;
        for (const auto& image : stage->images) {
            const std::uint64_t n = image.NumBindings(*stage);
            if (!n || n > max - current) return false;
            if (current == binding && n == count) {
                ++matches;
                absent = !image.is_written && image.GetSharp(*stage).Address() == 0;
            }
            current += n;
        }
    }
    return matches == 1 && absent;
}
}
