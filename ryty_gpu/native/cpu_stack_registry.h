// Fiber stack lifetime ranges and their page-aligned union. Mirror lookups use
// try-locks because a producer may already hold buffer_map_mutex.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <span>
#include <vector>

namespace Native {
class CpuStackRegistry {
public:
    using Address = std::uint64_t;
    struct Range { Address begin, end; };
    explicit CpuStackRegistry(bool cache_gap = false) : cache_gap_(cache_gap) {}
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
    class PointQuery {
    public:
        explicit PointQuery(CpuStackRegistry& owner) : owner_(owner) {}
        bool Allowed(Address begin, Address end) {
            if (!owner_.cache_gap_) return Lease{owner_}.Allowed(begin, end);
            return AllowedCached(begin, end);
        }
    private:
        bool AllowedCached(Address begin, Address end) {
            if (begin >= end) return false;
            const auto started = owner_.started_.load(std::memory_order_acquire);
            const bool same_revision = valid_ && stamp_ == started;
            if (same_revision) {
                if (gap_.begin <= begin && end <= gap_.end) return true;
                if (have_coverage_) return Check(saved_, begin, end, gap_);
            }
            std::shared_lock lock{owner_.mutex_, std::try_to_lock};
            if (!lock.owns_lock()) return false;
            const auto& coverage = owner_.coverage_;
            Range gap{};
            const bool allowed = Check(coverage, begin, end, gap);
            if (owner_.finished_.load(std::memory_order_acquire) == started) {
                have_coverage_ = false;
                if (same_revision) {
                    try {
                        saved_.assign(coverage.begin(), coverage.end());
                        have_coverage_ = true;
                    } catch (const std::bad_alloc&) {
                    }
                }
                gap_ = gap;
                stamp_ = started;
                valid_ = true;
            }
            return allowed;
        }
        static bool Check(std::span<const Range> coverage, Address begin, Address end, Range& gap) {
            const auto i = static_cast<std::size_t>(std::lower_bound(coverage.begin(), coverage.end(), begin,
                [](Range range, Address address) { return range.end <= address; }) - coverage.begin());
            if (i < coverage.size() && coverage[i].begin < end) return false;
            gap = {i == 0 ? 0 : coverage[i - 1].end,
                   i == coverage.size() ? std::numeric_limits<Address>::max() : coverage[i].begin};
            return true;
        }
        CpuStackRegistry& owner_;
        std::uint64_t stamp_{};
        Range gap_{};
        std::vector<Range> saved_;
        bool valid_{}, have_coverage_{};
    };
    template<class Invalidate>
    bool Register(Address begin, Address size, Invalidate&& invalidate) noexcept {
        if (!size || begin > std::numeric_limits<Address>::max() - size ||
            begin + size > std::numeric_limits<Address>::max() - 4095) return false;
        try {
            WriterChange change{*this};
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
        WriterChange change{*this};
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
    struct WriterChange {
        CpuStackRegistry& owner;
        explicit WriterChange(CpuStackRegistry& registry) : owner(registry) {
            if (owner.cache_gap_) owner.started_.fetch_add(1, std::memory_order_acq_rel);
        }
        ~WriterChange() {
            if (owner.cache_gap_) owner.finished_.fetch_add(1, std::memory_order_release);
        }
    };
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
    const bool cache_gap_;
    std::atomic<std::uint64_t> started_{}, finished_{};
};
inline CpuStackRegistry& CpuStacks() {
    static CpuStackRegistry registry{[] {
        const char* value = std::getenv("SHADGT_STACK_GAP_MEMO"); // PERF-SGT-228
        return value && value[0] == '1';
    }()};
    return registry;
}
} // namespace Native
