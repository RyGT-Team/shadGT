// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
namespace VideoCore {
// The map and every in-flight user share this owner. Release runs exactly once after the
// unmap completion gate and the last stream lease both drop their references.
class ImportedViewLease {
public:
    explicit ImportedViewLease(std::function<void()> release_) : release{std::move(release_)} {}
    ~ImportedViewLease() { if (release) release(); }
    ImportedViewLease(const ImportedViewLease&) = delete;
    ImportedViewLease& operator=(const ImportedViewLease&) = delete;
private:
    std::function<void()> release;
};

// Schedule owns the callback only on success. Failure leaves every lease with the caller.
template<class Schedule, class Completed>
bool RetireImportedLeases(std::vector<std::shared_ptr<void>>& leases, Schedule schedule,
                          Completed completed) {
    if (leases.empty()) return true;
    std::shared_ptr<std::vector<std::shared_ptr<void>>> held;
    try { held = std::make_shared<std::vector<std::shared_ptr<void>>>(); }
    catch (...) { return false; }
    held->swap(leases);
    try {
        schedule([held, completed]() mutable { completed(std::move(held)); });
    } catch (...) {
        held->swap(leases);
        return false;
    }
    return true;
}

// The caller holds the view lock and has established its coverage invalidation guard.
// Reserve ownership before removing any view; a failed retirement keeps the owners here.
template<class Map, class BeforeErase, class Retire>
void OnUnmapImportedViews(Map& views, std::uint64_t address, std::uint64_t end,
                          std::vector<std::shared_ptr<void>>& held, bool defer_until_splice,
                          BeforeErase before_erase, Retire retire) {
    held.reserve(held.size() + views.size());
    auto it = views.upper_bound(address);
    if (it != views.begin()) --it;
    while (it != views.end() && it->first < end) {
        auto& view = it->second;
        if (view.end <= address) { ++it; continue; }
        before_erase(view);
        held.push_back(std::move(view.lease));
        it = views.erase(it);
    }
    if (!defer_until_splice) retire(held);
}
}
