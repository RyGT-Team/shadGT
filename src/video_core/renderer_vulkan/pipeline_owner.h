#pragma once

#include <memory>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <unordered_set>
#include <utility>

namespace Vulkan {
template <typename Pipeline>
bool PipelineOwnerMatches(const Pipeline* pipeline, const std::shared_ptr<const void>& owner) {
    return pipeline && owner.get() == pipeline;
}

// Recorder-owned: a list holds one reference per distinct pipeline. A completed
// splice transfers those references to the GPU retirement tick.
class PipelineOwnerRetirement {
public:
    void Hold(const std::shared_ptr<const void>& owner) {
        if (!owner) return;
        if (last == owner.get()) return;
        auto [it, inserted] = identities.insert(owner.get());
        if (inserted) {
            try { active.push_back(owner); }
            catch (...) { identities.erase(it); throw; }
        }
        last = owner.get();
    }
    void Retire(std::uint64_t tick) {
        pending.reserve(pending.size() + active.size());
        for (auto& owner : active) pending.emplace_back(tick, std::move(owner));
        active.clear();
        identities.clear();
        last = nullptr;
    }
    template <typename Release>
    void Collect(std::uint64_t completed, Release release) {
        std::vector<std::shared_ptr<const void>> completed_owners;
        const auto count = std::count_if(pending.begin(), pending.end(),
            [completed](const auto& entry) { return entry.first <= completed && entry.second; });
        if (!count) return;
        completed_owners.reserve(static_cast<std::size_t>(count));
        for (auto& entry : pending) {
            if (entry.first <= completed && entry.second) {
                completed_owners.push_back(std::move(entry.second));
            }
        }
        std::erase_if(pending, [](const auto& entry) { return !entry.second; });
        release(std::move(completed_owners));
    }
    void ClearAfterGpuIdle() { active.clear(); pending.clear(); identities.clear(); last = nullptr; }
private:
    std::vector<std::shared_ptr<const void>> active;
    std::vector<std::pair<std::uint64_t, std::shared_ptr<const void>>> pending;
    std::unordered_set<const void*> identities;
    const void* last{};
};
} // namespace Vulkan
