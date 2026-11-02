// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
namespace Native {
// Non-owning thread-local lookups. Owners invalidate their epoch before deleting
// or replacing resources. Range users keep the original owner lock held.
template<class Value, std::size_t Capacity=16> class EpochValueMemo {
    struct Entry { const void* owner{}; std::uint64_t epoch{},key{}; Value value{}; };
    std::array<Entry,Capacity> entries{};
public:
    std::optional<Value> Find(const void* owner,std::uint64_t epoch,std::uint64_t key) const {
        const auto& entry=entries[key%Capacity];
        if(entry.owner==owner&&entry.epoch==epoch&&entry.key==key)return entry.value;
        return {};
    }
    void Store(const void* owner,std::uint64_t epoch,std::uint64_t key,Value value) {
        entries[key%Capacity]={owner,epoch,key,value};
    }
};
template<class Value> class EpochRangeMemo {
    const void* owner{};
    std::uint64_t epoch{},begin{},end{};
    Value value{};
public:
    struct Hit { Value value; std::uint64_t offset; };
    std::optional<Hit> Find(const void* who,std::uint64_t version,
                            std::uint64_t address,std::uint64_t size) const {
        // A zero-size lookup at end can select an adjacent view in the map.
        // Let the ordinary lookup decide that boundary case.
        if(owner==who&&epoch==version&&address>=begin&&address<end&&size<=end-address)
            return Hit{value,address-begin};
        return {};
    }
    void Store(const void* who,std::uint64_t version,std::uint64_t first,
               std::uint64_t last,Value result) {
        owner=who;epoch=version;begin=first;end=last;value=result;
    }
};
}
