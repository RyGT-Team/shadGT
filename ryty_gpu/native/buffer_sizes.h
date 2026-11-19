// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>

// PERF-SGT-020: the size each Vulkan buffer was created with, by handle. The recorder rounds
// the range of a buffer bound with a dynamic offset up to a power of two when that still ends
// inside the buffer, so draws whose buffers differ only in size share a kept descriptor set.
// A handle is forgotten before it is destroyed (where Native::DeadHandles notes it); one that
// was never noted has size 0 and its ranges are bound as they are.
namespace Native::BufferSizes {
inline std::mutex mutex;
inline std::unordered_map<std::uint64_t, std::uint64_t> sizes;

template <class Handle>
void Note(Handle handle, std::uint64_t size) {
    std::scoped_lock lock{mutex};
    sizes[reinterpret_cast<std::uint64_t>(handle)] = size;
}

template <class Handle>
void Forget(Handle handle) {
    std::scoped_lock lock{mutex};
    sizes.erase(reinterpret_cast<std::uint64_t>(handle));
}

inline std::uint64_t Find(std::uint64_t handle) {
    std::scoped_lock lock{mutex};
    const auto found = sizes.find(handle);
    return found == sizes.end() ? 0 : found->second;
}
} // namespace Native::BufferSizes
