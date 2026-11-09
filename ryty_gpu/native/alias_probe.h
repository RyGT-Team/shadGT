#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
namespace Native::AliasProbe {
constexpr std::uint64_t Page=0x4000, MaxRequest=64ull<<20, Budget=32;
enum class Status : std::uint64_t { Disabled, Invalid, MissingExport, BudgetExhausted, Refused, Success, UnmapFailed, Busy };
struct Result { Status status=Status::Disabled; std::uint64_t base=0, span=0, low_bits=0, ordinal=0, error=0; };
// No renderer import/cache state is touched; cleanup failure stops subsequent attempts.
template<class Map, class Unmap>
Result Try(bool enabled, std::uint64_t address, std::uint64_t size, std::atomic<std::uint32_t>& budget,
           std::atomic_flag& in_flight, Map map, bool cleanup_available, Unmap unmap) {
    Result r;
    if (!enabled) return r;
    if (in_flight.test_and_set(std::memory_order_acquire)) { r.status=Status::Busy; return r; }
    struct Release { std::atomic_flag& f; ~Release(){f.clear(std::memory_order_release);} } release{in_flight};
    auto n=budget.load(std::memory_order_relaxed);
    while(n<Budget && !budget.compare_exchange_weak(n,n+1,std::memory_order_relaxed)) {}
    if(n>=Budget) { r.status=Status::BudgetExhausted; return r; }
    r.ordinal=n+1;
    if (size < (4ull<<20) || size > MaxRequest || address > (1ull<<40) ||
        size > (1ull<<40)-address || address+size > std::numeric_limits<std::uint64_t>::max()-(Page-1)) {
        r.status=Status::Invalid; return r;
    }
    r.base=address & ~(Page-1);
    const auto end=(address+size+Page-1)&~(Page-1); r.span=end-r.base;
    if (!map || !cleanup_available) { r.status=Status::MissingExport; return r; }
    void* alias;
    try { alias=map(r.base,r.span); }
    catch (...) { r.status=Status::Refused; return r; }
    if(!alias) { r.status=Status::Refused; return r; }
    r.low_bits=reinterpret_cast<std::uintptr_t>(alias)&0xffffu;
    std::uint32_t error=0;
    if (unmap(alias,error)) r.status=Status::Success;
    else { r.error=error; r.status=Status::UnmapFailed; budget.store(Budget,std::memory_order_relaxed); }
    return r;
}
}
