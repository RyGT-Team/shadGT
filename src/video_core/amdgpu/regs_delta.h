// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <span>
#include <vector>

#include "common/types.h"

namespace AmdGpu {

/// PERF-031: the register blocks written since the last draw, so the recorder thread can bring
/// its own copy of the register file up to date with a draw's state instead of copying all of it.
/// A delta is a sequence of [first block, block count, contiguous values] records.
template <u32 NumDwords, u32 BlockDwords = 32>
class RegsDelta {
public:
    static_assert(NumDwords % BlockDwords == 0);
    static constexpr u32 NumBlocks = NumDwords / BlockDwords;
    static constexpr u32 RecordDwords = BlockDwords + 2;

    /// Marks the dwords [first, first + count) as written.
    void Mark(u32 first, u32 count) {
        if (count == 0 || first >= NumDwords) {
            return;
        }
        const u32 last = first + std::min(count, NumDwords - first) - 1;
        for (u32 block = first / BlockDwords; block <= last / BlockDwords; ++block) {
            dirty[block / 64] |= u64{1} << (block % 64);
        }
    }

    void MarkAll() {
        dirty.fill(~u64{0});
        if constexpr (NumBlocks % 64 != 0) {
            dirty.back() = (u64{1} << (NumBlocks % 64)) - 1;
        }
    }

    bool Any() const {
        return std::ranges::any_of(dirty, [](u64 word) { return word != 0; });
    }

    /// Appends runs of written blocks of `regs` to `out` and clears the marks.
    void Collect(std::span<const u32, NumDwords> regs, std::vector<u32>& out) {
        for (u32 word = 0; word < dirty.size(); ++word) {
            u64 bits = dirty[word];
            while (bits != 0) {
                const u32 first_bit = std::countr_zero(bits);
                const u32 block = word * 64 + first_bit;
                const u32 blocks = std::countr_one(bits >> first_bit);
                const u64 mask = blocks == 64 ? ~u64{0} : ((u64{1} << blocks) - 1) << first_bit;
                bits &= ~mask;
                out.push_back(block);
                out.push_back(blocks);
                const auto first = regs.begin() + block * BlockDwords;
                out.insert(out.end(), first, first + blocks * BlockDwords);
            }
            dirty[word] = 0;
        }
    }

    /// Writes the blocks of a delta made by Collect into `regs`.
    static void Apply(std::span<const u32> delta, std::span<u32, NumDwords> regs) {
        for (size_t at = 0; at + 2 <= delta.size();) {
            const u32 block = delta[at];
            const u32 blocks = delta[at + 1];
            if (block >= NumBlocks || blocks == 0 || blocks > NumBlocks - block) {
                break;
            }
            const size_t count = size_t{blocks} * BlockDwords;
            if (count > delta.size() - at - 2) break;
            std::memcpy(&regs[block * BlockDwords], &delta[at + 2], count * sizeof(u32));
            at += 2 + count;
        }
    }

private:
    std::array<u64, (NumBlocks + 63) / 64> dirty{};
};

} // namespace AmdGpu
