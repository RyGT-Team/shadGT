// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace Common {
struct FetchCodeSnapshot {
    std::uint32_t sgpr_base{};
    std::uintptr_t address{};
    std::vector<std::uint32_t> words;
};

// S_SWAPPC_B64 names the SGPR pair holding the auxiliary fetch program. Scan
// conservatively: a matching literal can reject speculative work, never permit
// a worker to fall back to a guest pointer. Reads include only complete fetch
// instructions through S_SETPC_B64, bounded to the parser's 1024-dword limit.
template <typename Read, typename InstructionWords>
bool SnapshotFetchPrograms(std::span<const std::uint32_t> main_code,
                           std::span<const std::uint32_t> user_data, Read&& read,
                           InstructionWords&& instruction_words,
                           std::vector<FetchCodeSnapshot>& output) {
    std::vector<FetchCodeSnapshot> captured;
    for (const auto word : main_code) {
        if ((word & 0xff80ff00u) != 0xbe802100u) continue;
        const std::uint32_t base = word & 0xff;
        if (base >= user_data.size() || user_data.size() - base < 2) return false;
        bool duplicate = false;
        for (const auto& entry : captured) duplicate |= entry.sgpr_base == base;
        if (duplicate) continue;
        const auto address = std::uintptr_t(std::uint64_t(user_data[base]) |
                                           (std::uint64_t(user_data[base + 1]) << 32));
        if (address < 0x10000 || (address & 3)) return false;
        FetchCodeSnapshot entry{base, address, {}};
        bool returned = false;
        while (entry.words.size() < 1024) {
            const auto offset = entry.words.size() * sizeof(std::uint32_t);
            if (address > (std::numeric_limits<std::uintptr_t>::max)() - offset) return false;
            std::uint32_t first{};
            if (!read(address + offset, first)) return false;
            const auto count = instruction_words(first);
            if (count == 0 || count > 2 || entry.words.size() + count > 1024) return false;
            entry.words.push_back(first);
            if (count == 2) {
                std::uint32_t second{};
                if (address + offset > (std::numeric_limits<std::uintptr_t>::max)() - 4 ||
                    !read(address + offset + 4, second)) return false;
                entry.words.push_back(second);
            }
            if ((first & 0xff80ff00u) == 0xbe802000u) {
                returned = true;
                break;
            }
        }
        if (!returned) return false;
        captured.push_back(std::move(entry));
    }
    output = std::move(captured);
    return true;
}
} // namespace Common
