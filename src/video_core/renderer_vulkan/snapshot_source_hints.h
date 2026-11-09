// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Vulkan {
// Hints identify candidates only; callers must validate the slot and compare its bytes.
template<std::size_t Slots>
class SnapshotSourceHints {
    struct Hint { std::uint64_t address{}, size{}; bool valid{}; };
    std::array<Hint, Slots> hints{};
public:
    void Clear() { hints = {}; }
    void Forget(std::size_t slot) { if (slot < Slots) hints[slot] = {}; }
    void Remember(std::size_t slot, std::uint64_t address, std::uint64_t size) {
        if (slot >= Slots) return;
        for (auto& hint : hints)
            if (hint.valid && hint.address == address && hint.size == size) hint = {};
        hints[slot] = {address, size, true};
    }
    std::optional<std::size_t> Candidate(std::uint64_t address, std::uint64_t size) const {
        for (std::size_t slot = 0; slot < Slots; ++slot)
            if (hints[slot].valid && hints[slot].address == address && hints[slot].size == size)
                return slot;
        return std::nullopt;
    }
};
} // namespace Vulkan
