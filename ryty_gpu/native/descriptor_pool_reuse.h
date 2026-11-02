// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <utility>
#include <tuple>
namespace Native {
// Caller holds the queue lock. Remove at most one completed pool; reset it only
// after releasing that lock. Already-free pools require no driver call at all.
template<class Free, class Retiring, class Tick>
auto TakeReadyDescriptorPool(Free& free, Retiring& retiring, Tick completed) {
    using Pool=typename Free::value_type;
    if (!free.empty()) {
        Pool pool=free.back();free.pop_back();return std::pair{pool,false};
    }
    if (!retiring.empty() && retiring.front().first <= completed) {
        Pool pool=retiring.front().second;retiring.pop_front();return std::pair{pool,true};
    }
    return std::pair{Pool{},false};
}
}
