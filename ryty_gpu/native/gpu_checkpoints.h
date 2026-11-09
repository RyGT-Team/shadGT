#pragma once
#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <vector>
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Native::GpuCheckpoints {
#ifndef NDEBUG
inline bool Requested() {
    static const bool requested = [] {
        const char* value = std::getenv("RYGT_GPU_CHECKPOINTS");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return requested;
}
inline std::atomic<bool> supported{};
inline std::atomic<uint64_t> sequence{};
struct Slot {
    std::atomic<uint64_t> stamp{}, kind{}, frame{}, program{}, companion{}, detail{};
};
// Allocate only if diagnostics are requested and a marker is recorded or read.
// At 20,000 markers per second this retains about 52 seconds of metadata.
inline constexpr size_t SlotCount = 1 << 20;
inline Slot* Slots() {
    static const auto slots = std::make_unique<Slot[]>(SlotCount);
    return slots.get();
}

inline void Record(VkCommandBuffer command, uint64_t kind, uint64_t frame,
                   uint64_t program, uint64_t companion, uint64_t detail) {
    if (!supported.load(std::memory_order_relaxed)) return;
    const auto set_checkpoint = VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdSetCheckpointNV;
    if (!set_checkpoint) return;
    const auto id = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    auto& slot = Slots()[id % SlotCount];
    slot.stamp.store(0, std::memory_order_release);
    slot.kind.store(kind, std::memory_order_relaxed);
    slot.frame.store(frame, std::memory_order_relaxed);
    slot.program.store(program, std::memory_order_relaxed);
    slot.companion.store(companion, std::memory_order_relaxed);
    slot.detail.store(detail, std::memory_order_relaxed);
    slot.stamp.store(id, std::memory_order_release);
    // Marker values are opaque IDs; the driver never dereferences this value.
    set_checkpoint(command, reinterpret_cast<const void*>(uintptr_t(id)));
}

inline void Log(VkQueue queue) {
    if (!supported.load(std::memory_order_relaxed)) return;
    if (!VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdSetCheckpointNV) {
        LOG_CRITICAL(Render_Vulkan, "GPU-CHECKPOINT: vkCmdSetCheckpointNV unavailable");
    }
    const auto get = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueueCheckpointDataNV;
    if (!get) {
        LOG_CRITICAL(Render_Vulkan, "GPU-CHECKPOINT: vkGetQueueCheckpointDataNV unavailable");
        return;
    }
    uint32_t count = 0;
    get(queue, &count, nullptr);
    if (count == 0) {
        LOG_CRITICAL(Render_Vulkan, "GPU-CHECKPOINT: driver returned zero checkpoints; recorded={}",
                     sequence.load(std::memory_order_relaxed));
        return;
    }
    if (count > 256) {
        LOG_CRITICAL(Render_Vulkan, "GPU-CHECKPOINT: refused invalid count {}", count);
        return;
    }
    std::vector<VkCheckpointDataNV> checkpoints(count);
    for (auto& checkpoint : checkpoints) checkpoint.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
    get(queue, &count, checkpoints.data());
    if (count == 0) {
        LOG_CRITICAL(Render_Vulkan, "GPU-CHECKPOINT: driver returned zero checkpoint records on second query; recorded={}",
                     sequence.load(std::memory_order_relaxed));
        return;
    }
    for (uint32_t i = 0; i < count && i < checkpoints.size(); ++i) {
        const auto& checkpoint = checkpoints[i];
        const auto id = uint64_t(uintptr_t(checkpoint.pCheckpointMarker));
        const auto& slot = Slots()[id % SlotCount];
        const auto before = slot.stamp.load(std::memory_order_acquire);
        const auto kind = slot.kind.load(std::memory_order_relaxed);
        const auto frame = slot.frame.load(std::memory_order_relaxed);
        const auto program = slot.program.load(std::memory_order_relaxed);
        const auto companion = slot.companion.load(std::memory_order_relaxed);
        const auto detail = slot.detail.load(std::memory_order_relaxed);
        const auto after = slot.stamp.load(std::memory_order_acquire);
        if (id != 0 && before == id && after == id) {
            LOG_CRITICAL(Render_Vulkan,
                "GPU-CHECKPOINT: stage={:#x} id={} kind={} frame={} program={:#x} companion={:#x} detail={:#x}",
                uint32_t(checkpoint.stage), id, kind, frame, program, companion, detail);
        } else {
            LOG_CRITICAL(Render_Vulkan, "GPU-CHECKPOINT: stage={:#x} id={} metadata expired; latest={} capacity={}",
                uint32_t(checkpoint.stage), id, sequence.load(std::memory_order_relaxed), SlotCount);
        }
    }
}
#else
inline constexpr bool Requested() noexcept { return false; }
inline void Record(VkCommandBuffer, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) noexcept {}
inline void Log(VkQueue) noexcept {}
#endif

}
