// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Common {

template <typename Allocate, typename Reclaim, typename Error>
auto AllocateWithPressureRetry(Allocate&& allocate, Reclaim&& reclaim, Error recoverable) {
    auto result = allocate();
    if (result.result == recoverable) {
        reclaim();
        result = allocate();
    }
    return result;
}

} // namespace Common
