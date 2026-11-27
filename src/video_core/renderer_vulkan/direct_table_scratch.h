// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

namespace Vulkan {

class DirectTableScratch {
public:
    using Row = std::array<std::uint32_t, 4>;
    static_assert(sizeof(Row) == 16);

    std::span<Row> Begin(std::size_t count, bool can_bind_direct) {
        if (single && !can_bind_direct) {
            return {};
        }
        rows.assign(count + std::size_t(single), Row{});
        return std::span{rows}.subspan(std::size_t(single));
    }

    template <class Memory>
    std::span<const Row> Pack(std::size_t count, std::span<const Row> entries, Memory* memory) {
        if (!memory) {
            packed.assign(count + 1, Row{});
            return packed;
        }
        const auto l1 = memory->L1Address();
        const auto zero = memory->ZeroPageAddress();
        const Row header{std::uint32_t(l1), std::uint32_t(l1 >> 32),
                         std::uint32_t(zero), std::uint32_t(zero >> 32)};
        if (single && !entries.empty()) {
            rows.front() = header;
            return rows;
        }
        packed.assign(count + 1, Row{});
        packed.front() = header;
        std::copy(entries.begin(), entries.end(), packed.begin() + 1);
        return packed;
    }

private:
    static bool Enabled() {
        const char* env = std::getenv("SHADGT_SINGLE_DIRECT_TABLE"); // PERF-SGT-219
        return env && env[0] == '1';
    }

    const bool single = Enabled();
    std::vector<Row> rows;
    std::vector<Row> packed;
};

} // namespace Vulkan
