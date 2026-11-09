#pragma once
#include <chrono>
#include <cstdint>

namespace Vulkan {
// Optional promotion only. Draws keep using their existing imported guest buffers
// until a complete mirror has been recorded and published in GPU order.
class OptionalMirrorBudget {
public:
    static constexpr std::uint32_t MaxStarts = 4;
    static constexpr std::uint64_t MaxBytes = 8ULL << 20;
    static constexpr auto MaxWork = std::chrono::milliseconds{1};

    void BeginFrame(std::uint64_t frame) {
        if (initialized && frame == current_frame) return;
        initialized = true;
        current_frame = frame;
        starts = 0;
        bytes = 0;
        work = {};
    }
    bool Available() const {
        return starts < MaxStarts && bytes < MaxBytes && work < MaxWork;
    }
    bool TryStart(std::uint64_t size) {
        if (!Available()) return false;
        // Permit one oversized promotion to make progress without breaking an
        // existing contiguous mirror into partial, unpublishable copies.
        if (starts != 0 && size > MaxBytes - bytes) return false;
        ++starts;
        bytes += size;
        return true;
    }
    void Account(std::chrono::steady_clock::duration elapsed) { work += elapsed; }
private:
    bool initialized{};
    std::uint64_t current_frame{}, bytes{};
    std::uint32_t starts{};
    std::chrono::steady_clock::duration work{};
};
}
