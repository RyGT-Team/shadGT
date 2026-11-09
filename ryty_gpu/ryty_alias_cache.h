// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cassert>
#include <cstdint>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Ryty {
// The caller serializes lookup, insertion and use. Removed alias pointers are
// returned to the caller for unmapping after releasing that synchronization.
template<std::size_t Capacity = 4096>
class AliasEntries {
    static_assert(Capacity > 0);
    using Order = std::list<std::pair<std::uint64_t, void*>>;
    Order order;
    std::unordered_map<std::uint64_t, typename Order::iterator> entries;
public:
    std::optional<void*> Find(std::uint64_t page) {
        const auto it = entries.find(page);
        if (it == entries.end()) return std::nullopt;
        order.splice(order.begin(), order, it->second);
        return it->second->second; // A cached failed mapping remains distinguishable.
    }
    void* Insert(std::uint64_t page, void* alias) {
        assert(!entries.contains(page));
        void* retired = nullptr;
        if (entries.size() == Capacity) {
            retired = order.back().second;
            entries.erase(order.back().first);
            order.pop_back();
        }
        order.emplace_front(page, alias);
        entries.emplace(page, order.begin());
        return retired;
    }
    void RemoveRange(std::uint64_t first, std::uint64_t end, std::vector<void*>& retired) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (it->first >= first && it->first < end) {
                if (it->second->second) retired.push_back(it->second->second);
                order.erase(it->second);
                it = entries.erase(it);
            } else ++it;
        }
    }
    std::size_t Size() const { return entries.size(); }
};
} // namespace Ryty
