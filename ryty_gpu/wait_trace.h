#pragma once
#include "image_lifecycle.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
namespace ImageLife {
// Thread-local, capture-only aggregates. A partial final bucket is intentionally omitted.
struct WaitAggregate {
    std::uint64_t count{}, total{}, maximum{}, busy{}, previous_owner{}, first{}, last{};
    void Add(const char* label, std::uint64_t owner, std::uint64_t role,
             std::uint64_t start, std::uint64_t end, bool is_busy=false, std::uint64_t depth=0) {
        if (!start || !end || end < start || !Enabled(label)) { count=total=maximum=busy=first=last=0; return; }
        if (owner != previous_owner) { count=total=maximum=busy=first=last=0; previous_owner=owner; }
        const auto elapsed=end-start;
        if (!count) first=start;
        last=end;
        ++count; total+=elapsed; maximum=std::max(maximum,elapsed); busy+=is_busy;
        if (elapsed >= SlowThresholdTicks()) {
            const char* detail = !std::strcmp(label,"direct_hold_summary") ? "direct_hold_detail" :
                                 !std::strcmp(label,"direct_wait_summary") ? "direct_wait_detail" : "queue_wait_detail";
            Scope slow{detail, owner, role, start, end, is_busy ? 1ull : 0ull};
            slow.Finish(depth,0);
        }
        if (count == 256) {
            Scope summary{label, owner, role, count, total, maximum};
            { Scope window{"wait_summary_window", summary.Id(), first, last}; }
            summary.Finish(busy, 0);
            count=total=maximum=busy=first=last=0;
        }
    }
};
struct DirectWaitTrace {
    static inline thread_local unsigned depth{};
    std::uint64_t owner, role, start{}, acquired{}, before{};
    bool busy{}, nested{}, tracking{};
    DirectWaitTrace(std::uint64_t o, std::uint64_t r) : owner(o), role(r) {
        nested=depth++ != 0;
        if (Enabled("direct_wait_summary")) { tracking=true; start=Timestamp(); }
    }
    void Acquired(bool ok=true) { if(start) { acquired=Timestamp(); busy=!ok; } }
    void BeforeUnlock() { if(start && !busy) before=Timestamp(); }
    ~DirectWaitTrace() {
        static thread_local WaitAggregate waits[4], holds[4];
        --depth;
        if (!tracking) return;
        if (nested || !start || !acquired) return;
        waits[role-1].Add("direct_wait_summary",owner,role,start,acquired,busy);
        if (before) holds[role-1].Add("direct_hold_summary",owner,role,acquired,before);
    }
};
struct DirectHoldStamp { DirectWaitTrace& trace; ~DirectHoldStamp() { trace.BeforeUnlock(); } };
struct QueueWaitBatch {
    struct Item { unsigned role; std::uint64_t start, end, depth; };
    std::array<WaitAggregate,5>& aggregate;
    std::uint64_t owner;
    std::array<Item,5> items{};
    unsigned size{};
    void Add(unsigned role, std::uint64_t start, std::uint64_t depth) {
        if (size < items.size()) items[size++]={role,start,start ? Timestamp() : 0,depth};
    }
    ~QueueWaitBatch() {
        for (unsigned i=0;i<size;++i) {
            const auto& item=items[i];
            aggregate[item.role-1].Add("queue_wait_summary",owner,item.role,item.start,item.end,false,item.depth);
        }
    }
};
}
