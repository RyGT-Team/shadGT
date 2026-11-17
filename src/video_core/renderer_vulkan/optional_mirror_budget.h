#pragma once
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace Vulkan {
// Optional promotion only. Draws keep using their existing imported guest buffers
// until a complete mirror has been recorded and published in GPU order.
class OptionalMirrorBudget {
public:
    // The limits a default-constructed budget has (the tests' and the renderer's before
    // PERF-GTS-525).
    static constexpr std::uint32_t MaxStarts = 4;
    static constexpr std::uint64_t MaxBytes = 8ULL << 20;
    static constexpr auto MaxWork = std::chrono::milliseconds{1};

    OptionalMirrorBudget() = default;
    OptionalMirrorBudget(std::uint32_t max_starts, std::uint64_t max_bytes,
                         std::chrono::microseconds max_work)
        : max_starts{max_starts}, max_bytes{max_bytes}, max_work{max_work} {}

    // PERF-GTS-525: the renderer's limits. With 4 mirrors, 8 MB and 1 ms a frame, a GT7 scene that
    // brought in hundreds of new vertex and index ranges at once was read in place over PCIe for
    // 10-15 s: 50-260 MB a frame bound in place while their mirrors waited, the GPU 95% busy at
    // 32 W and the game at 17-19 fps. SHADGT_MIRROR_PROMOTE_STARTS (32), _MB (64) and _US (4000)
    // set them; 4, 8 and 1000 are as before. Same GT7 scenes, 5 s windows: 16/32 MB/2 ms left
    // the worst at 166 MB a frame in place and about 45 fps; 32/64 MB/4 ms at 61 MB, about 48-50 fps,
    // the GPU never 85% busy, and no popping.
    static OptionalMirrorBudget FromEnvironment() {
        const auto read = [](const char* name, unsigned long long fallback) {
            const char* env = std::getenv(name);
            if (!env || !*env) return fallback;
            char* end = nullptr;
            const unsigned long long value = std::strtoull(env, &end, 10);
            return end != env && !*end && value != 0 ? value : fallback;
        };
        return OptionalMirrorBudget{
            static_cast<std::uint32_t>(read("SHADGT_MIRROR_PROMOTE_STARTS", 32)),
            read("SHADGT_MIRROR_PROMOTE_MB", 64) << 20,
            std::chrono::microseconds{read("SHADGT_MIRROR_PROMOTE_US", 4000)}};
    }

    void BeginFrame(std::uint64_t frame) {
        if (initialized && frame == current_frame) return;
        initialized = true;
        current_frame = frame;
        starts = 0;
        bytes = 0;
        work = {};
    }
    bool Available() const {
        return starts < max_starts && bytes < max_bytes && work < max_work;
    }
    bool TryStart(std::uint64_t size) {
        if (!Available()) return false;
        // Permit one oversized promotion to make progress without breaking an
        // existing contiguous mirror into partial, unpublishable copies.
        if (starts != 0 && size > max_bytes - bytes) return false;
        ++starts;
        bytes += size;
        return true;
    }
    void Account(std::chrono::steady_clock::duration elapsed) { work += elapsed; }
private:
    std::uint32_t max_starts{MaxStarts};
    std::uint64_t max_bytes{MaxBytes};
    std::chrono::steady_clock::duration max_work{MaxWork};
    bool initialized{};
    std::uint64_t current_frame{}, bytes{};
    std::uint32_t starts{};
    std::chrono::steady_clock::duration work{};
};
}
