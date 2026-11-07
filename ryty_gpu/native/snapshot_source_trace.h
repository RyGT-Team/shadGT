#pragma once
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NDEBUG
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace Native::SnapshotSourceTrace {
struct Context { uint64_t program{}; uint32_t index{}, binding{}; bool scalar_pointer{}, valid{}; };
inline thread_local Context current{};
inline bool Enabled() { static const bool on = [] { const char* e = std::getenv("RYGT_SNAPSHOT_SOURCE_TRACE"); return e && e[0] == '1'; }(); return on; }
struct Scope {
    Context previous;
    bool active;
    explicit Scope(Context next, bool enabled = Enabled()) : active(enabled) { if (active) { previous=current; current=next; } }
    ~Scope() { if (active) current = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
struct Classification { uint8_t memory{}, watch{}, stack{}, direct{}, imported{}; };
struct Row { uint64_t address{}, size{}, first_frame{}, last_frame{}, total_hits{}, calls{}, bytes{}; Context context{}; Classification classification{}; bool occupied{}; };
struct Table {
    std::array<Row, 128> rows{};
    uint64_t calls{}, samples{}, overflow{}, collision{};
    std::array<uint64_t, 5> memory_calls{}, memory_bytes{};
    std::array<uint64_t, 4> watch_calls{}, watch_bytes{}, imported_calls{}, imported_bytes{};
    std::array<uint64_t, 3> stack_calls{}, stack_bytes{}, direct_calls{}, direct_bytes{};
    std::array<uint64_t, 2> scalar_calls{}, scalar_bytes{};
    uint32_t random{0xa341316c};
    bool Select() { ++calls; random ^= random << 13; random ^= random >> 17; random ^= random << 5; if ((random & 255) != 0) return false; ++samples; return true; }
    void Add(uint64_t address, uint64_t size, uint64_t frame, Context context, Classification c) {
        ++memory_calls[c.memory]; memory_bytes[c.memory]+=size;
        ++watch_calls[c.watch]; watch_bytes[c.watch]+=size;
        ++stack_calls[c.stack]; stack_bytes[c.stack]+=size;
        ++direct_calls[c.direct]; direct_bytes[c.direct]+=size;
        ++imported_calls[c.imported]; imported_bytes[c.imported]+=size;
        ++scalar_calls[context.valid && context.scalar_pointer]; scalar_bytes[context.valid && context.scalar_pointer]+=size;
        const auto hash = (address ^ (address >> 17) ^ size ^ context.program ^ context.binding) % rows.size();
        for (size_t i=0;i<rows.size();++i) { auto& row=rows[(hash+i)%rows.size()];
            if (!row.occupied) { row.occupied=true; row.address=address; row.size=size; row.first_frame=frame; row.context=context; }
            else if (row.address!=address || row.size!=size || row.context.program!=context.program || row.context.index!=context.index || row.context.binding!=context.binding || row.context.scalar_pointer!=context.scalar_pointer || row.context.valid!=context.valid) { ++collision; continue; }
            row.last_frame=frame; ++row.total_hits; ++row.calls; row.bytes+=size; row.classification=c; return;
        } ++overflow;
    }
    void ResetInterval() { auto saved_random=random; *this=Table{}; random=saved_random; }
};
inline thread_local Table table{};
}
#endif
