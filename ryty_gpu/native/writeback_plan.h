// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <utility>
#include <algorithm>
#include <stdexcept>
#include <memory>
#include <vector>
#include <cstdint>

namespace Native {
inline bool RegionWritebackNeedsFallback(std::uint64_t overlap, bool direct, bool shader,
                                         bool pipeline, bool push_bindings, bool full_page) {
    return !direct || overlap > 4 * 1024 * 1024 ||
           (shader && (!pipeline || !push_bindings || !full_page));
}
template<class Pages, class Forget>
void ForgetWritebackPages(Pages& pages, Forget forget) {
    for (const auto& page : pages) forget(page);
    pages.clear();
}
template<class Parts, class Address>
bool WritebackPageAlreadyPlanned(const Parts& parts, Address region_begin, Address page) {
    return std::any_of(parts.begin(), parts.end(), [&](const auto& part) {
        return part.region_begin == region_begin && part.page == page;
    });
}
template<class Vector>
void ReserveAdditional(Vector& values, typename Vector::size_type additional) {
    if (additional > values.max_size() - values.size()) throw std::length_error("writeback reserve");
    const auto needed = values.size() + additional;
    if (needed <= values.capacity()) return;
    const auto grown = values.capacity() > values.max_size() / 2 ? values.max_size() :
                       std::max<typename Vector::size_type>(8, values.capacity() * 2);
    values.reserve(std::max(needed, grown));
}
class RetiredWritebackPlans {
public:
    template<class Reserve>
    bool Retire(std::vector<std::shared_ptr<void>>& active, std::uint64_t tick, Reserve reserve) noexcept {
        try { reserve(pending, active.size()); }
        catch (...) { return false; }
        for (auto& owner : active) pending.emplace_back(tick, std::move(owner));
        active.clear(); // keep capacity for the next stream
        return true;
    }
    bool Retire(std::vector<std::shared_ptr<void>>& active, std::uint64_t tick) noexcept {
        return Retire(active, tick, [](auto& entries, auto count) { ReserveAdditional(entries, count); });
    }
    void Collect(std::uint64_t completed) {
        std::erase_if(pending, [completed](const auto& item) { return item.first <= completed; });
    }
    void ClearAfterGpuIdle() { pending.clear(); }
private:
    std::vector<std::pair<std::uint64_t, std::shared_ptr<void>>> pending;
};
// Nothing is published to the caller until every range has been prepared.
template<class Plan, class Ranges, class Prepare>
bool PrepareWritebackOperation(Plan& result, const Ranges& ranges, Prepare prepare) {
    Plan candidate;
    for (const auto& range : ranges) {
        if (!prepare(candidate, range)) return false;
    }
    result = std::move(candidate);
    return true;
}
}
