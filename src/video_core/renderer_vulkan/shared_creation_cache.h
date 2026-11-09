#pragma once
#include <future>
#include <map>
#include <memory>
#include <mutex>
namespace Vulkan {
// Deduplicate creation without holding a global lock during compilation or waits.
// Completed entries are weak: the cache never extends a GPU resource's lifetime.
template<class Key, class Value>
class SharedCreationCache {
public:
    using Pointer = std::shared_ptr<Value>;
    Pointer Find(const Key& key) {
        std::scoped_lock lock{mutex};
        const auto it = ready.find(key);
        return it == ready.end() ? nullptr : it->second.lock();
    }
    // A consumer can overlap other work with an existing creation without
    // launching a thread whose only task would be future.get().
    std::shared_future<Pointer> Pending(const Key& key) {
        std::scoped_lock lock{mutex};
        const auto it = pending.find(key);
        return it == pending.end() ? std::shared_future<Pointer>{} : it->second;
    }
    template<class Factory> Pointer GetOrCreate(const Key& key, Factory&& factory) {
        std::shared_future<Pointer> future;
        std::shared_ptr<std::promise<Pointer>> owner;
        {
            std::scoped_lock lock{mutex};
            if (const auto it = ready.find(key); it != ready.end()) {
                if (auto value = it->second.lock()) return value;
            }
            if (const auto it = pending.find(key); it != pending.end()) {
                future = it->second;
            } else {
                owner = std::make_shared<std::promise<Pointer>>();
                future = owner->get_future().share();
                pending.emplace(key, future);
            }
        }
        if (!owner) return future.get();
        try {
            Pointer value = factory();
            {
                std::scoped_lock lock{mutex};
                ready[key] = value;
                pending.erase(key);
                if (++insertions % 128 == 0) {
                    std::erase_if(ready, [](const auto& item) { return item.second.expired(); });
                }
            }
            owner->set_value(value);
            return value;
        } catch (...) {
            const auto error = std::current_exception();
            {
                std::scoped_lock lock{mutex};
                pending.erase(key);
            }
            owner->set_exception(error);
            std::rethrow_exception(error);
        }
    }
private:
    std::mutex mutex;
    std::map<Key, std::weak_ptr<Value>> ready;
    std::map<Key, std::shared_future<Pointer>> pending;
    unsigned insertions = 0;
};
}
