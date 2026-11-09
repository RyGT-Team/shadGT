// SPDX-FileCopyrightText: Copyright 2026 RyGT Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace Vulkan {

// Will Scarlett OPT-014 / PERF-GT7-006: a command list need not reissue
// identical bindings. Values are copied; no per-draw payload pointer survives.
// The caller invalidates after external command recording or list rotation.
template <class Layout, class Set, class Push, std::size_t Sets>
class BindingCommandMemo {
    static_assert(std::is_trivially_copyable_v<Push>);
public:
    void Invalidate() noexcept {
        layout_known = push_known = false;
        set_known.fill(false);
        pushed_known.fill(false);
    }
    void Prepare(Layout current, std::uint32_t point, std::uint64_t generation,
                 std::uint64_t commands = 0) noexcept {
        if (!layout_known || current != layout || point != bind_point || generation != epoch ||
            commands != command_epoch) {
            Invalidate();
            layout = current;
            bind_point = point;
            epoch = generation;
            command_epoch = commands;
            layout_known = true;
        }
    }
    bool SamePush(const Push& value) const noexcept {
        return push_known && std::memcmp(&push, &value, sizeof(Push)) == 0;
    }
    void NotePush(const Push& value) noexcept {
        std::memcpy(&push, &value, sizeof(Push));
        push_known = true;
    }
    bool SameSet(std::size_t index, Set value) const noexcept {
        return index < Sets && set_known[index] && sets[index] == value;
    }
    void NoteSet(std::size_t index, Set value) noexcept {
        if (index >= Sets) return;
        sets[index] = value;
        set_known[index] = true;
        pushed_known[index] = false;
    }
    bool SamePushed(std::size_t index, std::span<const std::uint64_t> words) const noexcept {
        return index < Sets && pushed_known[index] && pushed[index].size() == words.size() &&
               std::equal(words.begin(), words.end(), pushed[index].begin());
    }
    void NotePushed(std::size_t index, std::span<const std::uint64_t> words) {
        if (index >= Sets) return;
        pushed[index].assign(words.begin(), words.end());
        pushed_known[index] = true;
        set_known[index] = false;
    }
    void ForgetSet(std::size_t index) noexcept {
        if (index < Sets) set_known[index] = pushed_known[index] = false;
    }
private:
    Layout layout{};
    std::uint32_t bind_point{};
    std::uint64_t epoch{};
    std::uint64_t command_epoch{};
    bool layout_known{}, push_known{};
    Push push{};
    std::array<Set, Sets> sets{};
    std::array<bool, Sets> set_known{}, pushed_known{};
    std::array<std::vector<std::uint64_t>, Sets> pushed;
};
} // namespace Vulkan
