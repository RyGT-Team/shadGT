// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>

namespace Vulkan {
struct TessLdsLayout { std::uint64_t size, written; };
// Reader mode 1 starts at a group boundary; mode 2 starts at its own patch.
// Bound every reader to one group and reserve its full reachable tail.
constexpr std::optional<TessLdsLayout> TessLdsScratchLayout(std::uint64_t indices,
    std::uint64_t instances, std::uint64_t cp, std::uint64_t stride,
    std::uint64_t group_patches, std::uint32_t mode) {
    constexpr std::uint64_t limit = 32ull << 20;
    group_patches = std::max<std::uint64_t>(group_patches, 1);
    if (instances != 1 || !cp || !stride || (mode != 1 && mode != 2) ||
        stride > 512 || (stride & 15) || cp > 32 || cp > limit / stride || group_patches > limit / cp)
        return {};
    const auto patches = indices / cp;
    if (!patches || patches > limit / cp) return {};
    const auto vertices = patches * cp;
    const auto group = group_patches * cp;
    std::uint64_t reserved{};
    if (mode == 1) {
        if (patches > limit - (group_patches - 1)) return {};
        const auto groups = (patches + group_patches - 1) / group_patches;
        if (groups > limit / group) return {};
        reserved = groups * group;
    } else {
        if (vertices - cp > limit - group) return {};
        reserved = vertices - cp + group;
    }
    if (reserved > limit / stride) return {};
    return TessLdsLayout{reserved * stride, vertices * stride};
}
// Scratch aliases the quad mask. Select an unmodified TCS for every nontrivial
// raw mask, even when a later guest-specific override changes the pushed mask.
constexpr std::uint32_t TessLdsModeForMask(std::uint32_t mode, std::uint32_t low,
                                           std::uint32_t high) {
    return low == ~std::uint32_t{0} && high == ~std::uint32_t{0} ? mode : 0;
}
// The writer indexes by primitive, so instances would alias each other's scratch.
constexpr bool TessLdsSafeInstanceCount(std::uint64_t instances) { return instances == 1; }
constexpr bool TessLdsSafeShape(std::uint64_t instances, bool quad_triangles,
                                bool readonly_stages, bool writable_binding,
                                bool unmasked_fragment = true) {
    return TessLdsSafeInstanceCount(instances) && !quad_triangles && readonly_stages &&
           !writable_binding && unmasked_fragment;
}
// Returns a bounded allocation size without overflowing at any intermediate step.
constexpr std::optional<std::uint64_t> TessLdsScratchSize(std::uint64_t indices,
    std::uint64_t instances, std::uint64_t control_points, std::uint64_t vertex_bytes) {
    constexpr std::uint64_t limit = 32ull << 20;
    const auto cp = std::max<std::uint64_t>(control_points, 1);
    const auto vertex = std::max<std::uint64_t>(vertex_bytes, 64);
    if (cp > limit / vertex) return {};
    const auto patch_bytes = cp * vertex;
    const auto patches_per_instance = indices / cp;
    if (instances && patches_per_instance > limit / patch_bytes / instances) return {};
    const auto patches = patches_per_instance * instances;
    return std::max<std::uint64_t>(patches, 1) * patch_bytes;
}
}
