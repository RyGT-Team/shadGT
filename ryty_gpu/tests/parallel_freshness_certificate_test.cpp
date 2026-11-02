#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>
#include "video_core/renderer_vulkan/freshness_range_search.h"

using u64 = std::uint64_t;
using VAddr = u64;
using Vulkan::FreshnessRangeSearch;
#include "freshness-log-under-test.inc"

struct Prepared {
    u64 unmap_gen{}, buffer_seq{};
    std::vector<std::pair<VAddr, VAddr>> buffer_ranges;
};

static bool admitted(ChangeLog& buffer_log, Prepared* prepared, u64 current_unmap,
                     bool from_prepared, bool buffer_freshness_certified, u64 buffer_freshness_seq) {
    std::atomic<u64> unmap_generation{current_unmap};
    bool fast_reject = false;
#include "freshness-admission-under-test.inc"
    return !fast_reject;
}

// Extract both checks from CommitPrepared, with a hook representing ordered work.
static int commit_checks(ChangeLog& buffer_log, Prepared& p, std::atomic<u64>& unmap_generation,
                         const std::function<void()>& between, const std::function<void()>& before_first=[]{}) {
    const auto reject=[](int reason){return reason;};
#include "freshness-first-under-test.inc"
    between();
#include "freshness-recheck-under-test.inc"
    return 0;
}

int main() {
    ChangeLog log;
    Prepared p{7, log.Seq(), {{100, 200}}};
    assert(log.Untouched(p.buffer_seq, p.buffer_ranges));
    u64 certificate = log.Seq();
    assert(admitted(log, &p, 7, true, true, certificate));
    assert(!admitted(log, &p, 8, true, true, certificate));
    log.Note(300, 400); // A disjoint change must take and pass the full scan.
    assert(admitted(log, &p, 7, true, true, certificate));
    assert(admitted(log, &p, 7, true, false, certificate));
    log.Note(150, 160);
    assert(!admitted(log, &p, 7, true, true, certificate));

    ChangeLog promotion;
    Prepared q{3, promotion.Seq(), {{10, 20}}};
    certificate = promotion.Seq();
    promotion.Note(15, 16); // Vertex promotion invalidates the prior proof.
    assert(!promotion.Untouched(q.buffer_seq, q.buffer_ranges));
    assert(!admitted(promotion, &q, 3, true, true, certificate));
    q.buffer_seq = promotion.Seq(); // A newly prepared and checked draw may recertify.
    assert(promotion.Untouched(q.buffer_seq, q.buffer_ranges));
    certificate = promotion.Seq();
    assert(admitted(promotion, &q, 3, true, true, certificate));

    // Earlier disjoint writes are certified by the first scan; writes in the
    // intervening ordered work still need checking, including ring overflow.
    {
        ChangeLog changes;Prepared draw{5,changes.Seq(),{{100,200}}};std::atomic<u64> unmaps{5};
        for(int i=0;i<1000;++i)changes.Note(300,400);
        assert(commit_checks(changes,draw,unmaps,[&]{changes.Note(350,360);})==0);
        assert(commit_checks(changes,draw,unmaps,[&]{changes.Note(150,160);})==4);
    }
    {
        ChangeLog changes;Prepared draw{5,changes.Seq(),{{100,200}}};std::atomic<u64> unmaps{5};
        for(int i=0;i<1000;++i)changes.Note(300,400);
        assert(commit_checks(changes,draw,unmaps,[&]{for(int i=0;i<30;++i)changes.Note(300,400);})==0);
        assert(!changes.Untouched(draw.buffer_seq,draw.buffer_ranges)); // old second scan conservatively overflowed
    }
    {
        ChangeLog changes;Prepared draw{5,changes.Seq(),{{100,200}}};std::atomic<u64> unmaps{5};
        assert(commit_checks(changes,draw,unmaps,[&]{for(int i=0;i<1025;++i)changes.Note(300,400);})==4);
    }
    {
        ChangeLog changes;Prepared draw{5,changes.Seq(),{{100,200}}};std::atomic<u64> unmaps{5};
        assert(commit_checks(changes,draw,unmaps,[]{},[&]{changes.Note(150,160);})==4);
    }
    {
        ChangeLog changes;Prepared draw{5,changes.Seq(),{{100,200}}};std::atomic<u64> unmaps{5};
        assert(commit_checks(changes,draw,unmaps,[&]{changes.Note(150,160);},[&]{changes.Note(300,400);})==4);
    }
    {
        ChangeLog changes;Prepared draw{5,changes.Seq(),{{100,200}}};std::atomic<u64> unmaps{5};
        changes.Note(150,160);assert(commit_checks(changes,draw,unmaps,[]{assert(false);})==4);
    }
    {
        ChangeLog changes;Prepared draw{5,changes.Seq(),{{100,200}}};std::atomic<u64> unmaps{5};
        assert(commit_checks(changes,draw,unmaps,[&]{++unmaps;})==2);
    }
    ChangeLog overflow;
    Prepared old{1, overflow.Seq(), {{10, 20}}};
    certificate = overflow.Seq();
    for (int i = 0; i < 1025; ++i) overflow.Note(30, 40);
    assert(!admitted(overflow, &old, 1, true, true, certificate));
}
