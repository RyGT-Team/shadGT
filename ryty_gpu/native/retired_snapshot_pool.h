// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace Native {
// Buffers may be reused only after the submission that read them has completed.
template <class Buffer>
class RetiredSnapshotPool {
public:
    bool Empty() const { return buffers.empty(); }
    std::unique_ptr<Buffer> Take(std::uint64_t bytes, std::uint64_t completed_tick) {
        for (auto it = buffers.begin(); it != buffers.end(); ++it) {
            if (it->tick <= completed_tick && it->buffer->Size() == bytes) {
                auto result = std::move(it->buffer);
                retained_bytes -= result->Size();
                buffers.erase(it);
                return result;
            }
        }
        return {};
    }
    // A rejected buffer remains owned by the caller for normal deferred destruction.
    bool Retire(std::unique_ptr<Buffer>& buffer, std::uint64_t tick) {
        if (!buffer || !tick || buffer->Size() > Limit - retained_bytes) return false;
        try {
            if (buffers.size() == buffers.capacity())
                buffers.reserve(buffers.empty() ? 8 : buffers.size() * 2);
        } catch (...) {
            return false;
        }
        const auto bytes = buffer->Size();
        buffers.push_back({std::move(buffer), tick});
        retained_bytes += bytes;
        return true;
    }
private:
    static constexpr std::uint64_t Limit = 256ull * 1024 * 1024;
    struct Retired { std::unique_ptr<Buffer> buffer; std::uint64_t tick; };
    std::vector<Retired> buffers;
    std::uint64_t retained_bytes{};
};
} // namespace Native
