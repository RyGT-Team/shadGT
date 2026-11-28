// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/types.h"

namespace AmdGpu {

/// PERF-SGT-245: what the command processor knew about a dispatch when it handed it to the
/// recorder. Set on the recorder thread by the dispatch's job, around the renderer's call.
/// `unsynced`: the guest queue sent no packet that orders work (event, wait, release, acquire,
/// memory write or copy) since its previous dispatch, so on the PS4 the two may run at once and
/// the game cannot depend on their order. `queue`: 0 for graphics, vqid + 1 for compute;
/// ~0u when unknown.
struct GuestDispatch {
    u32 queue = ~0u;
    bool unsynced = false;
};

inline thread_local GuestDispatch t_guest_dispatch;

/// Sets t_guest_dispatch for the scope of one dispatch call.
class GuestDispatchScope {
public:
    GuestDispatchScope(u32 queue, bool unsynced) {
        t_guest_dispatch = {queue, unsynced};
    }
    ~GuestDispatchScope() {
        t_guest_dispatch = {};
    }
    GuestDispatchScope(const GuestDispatchScope&) = delete;
    GuestDispatchScope& operator=(const GuestDispatchScope&) = delete;
};

} // namespace AmdGpu
