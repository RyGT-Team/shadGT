#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace SceneResidency {
struct Region {
    std::uint64_t begin{}, end{};
    std::uint32_t state{}, protect{}, type{};
};
struct Snapshot {
    std::uint64_t begin{}, end{}, observed_end{};
    bool invalid{}, query_failed{};
    std::vector<Region> regions;
    bool Complete() const { return !invalid && !query_failed && observed_end == end; }
};
struct Coverage {
    std::uint64_t observed{}, committed{};
    bool complete{};
};
template <class Query>
Snapshot Observe(std::uint64_t begin, std::uint64_t size, Query&& query,
                 std::size_t limit = 256) {
    Snapshot out{.begin=begin,.end=begin,.observed_end=begin};
    if (!size || size > std::numeric_limits<std::uint64_t>::max() - begin) {
        out.invalid=true;
        return out;
    }
    out.end=begin+size;
    out.regions.reserve(std::min<std::size_t>(limit, 32));
    while (out.observed_end < out.end && out.regions.size() < limit) {
        Region region;
        if (!query(out.observed_end, region) || region.begin > out.observed_end ||
            region.end <= out.observed_end) {
            out.query_failed=true;
            break;
        }
        region.begin=out.observed_end;
        region.end=std::min(region.end,out.end);
        out.observed_end=region.end;
        out.regions.push_back(region);
    }
    return out;
}
inline Coverage Cover(const Snapshot& snapshot, std::uint64_t begin, std::uint64_t size) {
    Coverage out;
    if (!size || size > std::numeric_limits<std::uint64_t>::max()-begin) return out;
    for (const auto& region:snapshot.regions) {
        const auto lo=std::max(begin,region.begin), hi=std::min(begin+size,region.end);
        if (hi<=lo) continue;
        out.observed+=hi-lo;
        if (region.state==0x1000) out.committed+=hi-lo; // MEM_COMMIT, including tracked PAGE_NOACCESS.
    }
    out.complete=out.observed==size;
    return out;
}
}
