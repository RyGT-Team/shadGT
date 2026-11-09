// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <mutex>

namespace Vulkan {
// Invoke holds the registry lock through the callback. Unregister before taking owner locks.
template<class Owner> class ExitSaveRegistry {
public:
    bool Register(Owner* owner) {
        std::scoped_lock lock{mutex};
        if (!owner || current) return false;
        current=owner;
        return true;
    }
    void Unregister(Owner* owner) {
        std::scoped_lock lock{mutex};
        if (current==owner) current=nullptr;
    }
    template<class Fn> bool Invoke(Fn&& fn) {
        std::scoped_lock lock{mutex};
        if (!current) return false;
        fn(*current);
        return true;
    }
private:
    std::mutex mutex;
    Owner* current{};
};
}
