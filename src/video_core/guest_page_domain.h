// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>
namespace VideoCore {
inline constexpr std::uint64_t GuestTrackedAddressLimit=std::uint64_t{1}<<40;
constexpr bool IsTrackedGuestAddress(std::uint64_t address) {
    return address<GuestTrackedAddressLimit;
}
template<class Table>
auto FindTrackedGuestPage(Table& table,std::uint64_t address,std::size_t page_bits)
    -> decltype(table.find(address>>page_bits)) {
    if(!IsTrackedGuestAddress(address))return nullptr;
    return table.find(address>>page_bits);
}
}
