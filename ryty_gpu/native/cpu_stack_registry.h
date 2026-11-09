// Fiber stack lifetime ranges and their page-aligned union. Mirror lookups use
// try-locks because a producer may already hold buffer_map_mutex.
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace Native {
class CpuStackRegistry {
public:
    using Address = std::uint64_t;
    struct Range { Address begin, end; };
    class Lease {
    public:
        explicit Lease(CpuStackRegistry& owner) : owner_(owner), lock_(owner.mutex_, std::try_to_lock) {}
        bool Allowed(Address begin, Address end) const {
            if (!lock_.owns_lock() || begin >= end) return false;
            const auto it = std::lower_bound(owner_.coverage_.begin(), owner_.coverage_.end(), begin,
                [](Range range, Address address) { return range.end <= address; });
            return it == owner_.coverage_.end() || it->begin >= end;
        }
    private:
        CpuStackRegistry& owner_;
        std::shared_lock<std::shared_mutex> lock_;
    };
    template<class Invalidate>
    bool Register(Address begin, Address size, Invalidate&& invalidate) noexcept {
        if (!size || begin > std::numeric_limits<Address>::max() - size ||
            begin + size > std::numeric_limits<Address>::max() - 4095) return false;
        try {
            std::unique_lock lock{mutex_};
            auto next = ranges_;
            next.push_back({begin, begin + size});
            auto coverage = BuildCoverage(next);
            ranges_.swap(next);
            coverage_.swap(coverage);
            try { invalidate(); } catch (...) {
                ranges_.swap(next);
                coverage_.swap(coverage);
                return false;
            }
            return true;
        } catch (...) { return false; }
    }
    void Unregister(Address begin, Address size) {
        if (!size || begin > std::numeric_limits<Address>::max() - size) return;
        std::unique_lock lock{mutex_};
        auto next = ranges_;
        auto it = std::find_if(next.begin(), next.end(), [&](Range r) {
            return r.begin == begin && r.end == begin + size;
        });
        if (it == next.end()) return;
        next.erase(it);
        auto coverage = BuildCoverage(next);
        ranges_.swap(next);
        coverage_.swap(coverage);
    }
private:
    static std::vector<Range> BuildCoverage(const std::vector<Range>& ranges) {
        std::vector<Range> out;
        out.reserve(ranges.size());
        for (const Range r : ranges) out.push_back({r.begin & ~Address{4095}, (r.end + 4095) & ~Address{4095}});
        std::sort(out.begin(), out.end(), [](Range a, Range b) { return a.begin < b.begin; });
        std::size_t n = 0;
        for (const Range r : out) {
            if (n && r.begin <= out[n-1].end) out[n-1].end = std::max(out[n-1].end, r.end);
            else out[n++] = r;
        }
        out.resize(n);
        return out;
    }
    std::shared_mutex mutex_;
    std::vector<Range> ranges_;
    std::vector<Range> coverage_;
};
inline CpuStackRegistry& CpuStacks() {
    static CpuStackRegistry registry;
    return registry;
}
} // namespace Native
