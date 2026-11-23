// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

#include "common/types.h"

namespace Vulkan {

namespace StoredPipelineSlack {

// The bytes, in an inline vector's object, of its slots past its count; false when the count is
// beyond its slots. `loaded` is that object as read back from the stored bytes.
template <typename Vector>
bool FreeSlots(const Vector& loaded, size_t& from, size_t& to) {
    using T = typename Vector::value_type;
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* object = reinterpret_cast<const u8*>(std::addressof(loaded));
    const auto* slots = reinterpret_cast<const u8*>(loaded.data());
    if (slots < object || loaded.size() > loaded.capacity()) {
        return false;
    }
    const size_t storage = size_t(slots - object);
    if (storage + loaded.capacity() * sizeof(T) > sizeof(Vector)) {
        return false;
    }
    from = storage + loaded.size() * sizeof(T);
    to = storage + loaded.capacity() * sizeof(T);
    return true;
}

} // namespace StoredPipelineSlack

// FIX-SGT-135: a stored graphics pipeline file (RegisterPipelineData: u32 version, u32 kind, the
// key, then SerializationSupport::Serialize) holds its three vertex input tables and its
// multisample state as their objects' bytes. The table slots past each count, and the padding after
// the multisample state's sType, are whatever memory was there when the pipeline was built. The
// pipeline recipes (FEAT-GTS-471, ExportPipelineRecipes) are these files, made to be shared with
// other GPUs: each one carried this PC's memory, 1,152 bytes with empty tables (GT7, 1,661 recipes:
// 373 KB not zero, about 4,000 values shaped as this PC's addresses, 27 pieces of text). Nothing
// reads those bytes. This zeroes them in the file of a pipeline without helper shaders (the only
// kind exported); `loaded` is what LoadStoredGraphicsPipeline read from that file. False, the file
// untouched, when its length is not that of such a pipeline, a table's count is beyond its slots,
// or the multisample state holds a pointer (no build stores one).
template <typename Key, typename Support>
bool ClearStoredPipelineSlack(std::span<u8> file, const Support& loaded) {
    using Attributes = decltype(Support::vertex_attributes);
    using Bindings = decltype(Support::vertex_bindings);
    using Divisors = decltype(Support::divisors);
    using Multisample = decltype(Support::multisampling);
    static_assert(std::is_trivially_copyable_v<Multisample>);
    constexpr size_t attributes_at = 2 * sizeof(u32) + sizeof(Key);
    constexpr size_t bindings_at = attributes_at + sizeof(Attributes);
    constexpr size_t divisors_at = bindings_at + sizeof(Bindings);
    constexpr size_t multisample_at = divisors_at + sizeof(Divisors);
    // Then the helper shaders' two vectors, empty: their sizes alone.
    constexpr size_t length = multisample_at + sizeof(Multisample) + 2 * sizeof(size_t);
    if (file.size() != length || loaded.multisampling.pNext != nullptr ||
        loaded.multisampling.pSampleMask != nullptr) {
        return false;
    }
    std::array<std::pair<size_t, size_t>, 3> free{};
    if (!StoredPipelineSlack::FreeSlots(loaded.vertex_attributes, free[0].first, free[0].second) ||
        !StoredPipelineSlack::FreeSlots(loaded.vertex_bindings, free[1].first, free[1].second) ||
        !StoredPipelineSlack::FreeSlots(loaded.divisors, free[2].first, free[2].second)) {
        return false;
    }
    const std::array<size_t, 3> tables{attributes_at, bindings_at, divisors_at};
    for (size_t i = 0; i < tables.size(); ++i) {
        std::memset(file.data() + tables[i] + free[i].first, 0, free[i].second - free[i].first);
    }
    // Every byte of the multisample state that is none of its fields.
    constexpr std::array<std::pair<size_t, size_t>, 9> fields{{
        {offsetof(Multisample, sType), sizeof(Multisample::sType)},
        {offsetof(Multisample, pNext), sizeof(Multisample::pNext)},
        {offsetof(Multisample, flags), sizeof(Multisample::flags)},
        {offsetof(Multisample, rasterizationSamples), sizeof(Multisample::rasterizationSamples)},
        {offsetof(Multisample, sampleShadingEnable), sizeof(Multisample::sampleShadingEnable)},
        {offsetof(Multisample, minSampleShading), sizeof(Multisample::minSampleShading)},
        {offsetof(Multisample, pSampleMask), sizeof(Multisample::pSampleMask)},
        {offsetof(Multisample, alphaToCoverageEnable), sizeof(Multisample::alphaToCoverageEnable)},
        {offsetof(Multisample, alphaToOneEnable), sizeof(Multisample::alphaToOneEnable)},
    }};
    for (size_t at = 0; at < sizeof(Multisample); ++at) {
        bool field = false;
        for (const auto& [start, size] : fields) {
            field = field || (at >= start && at < start + size);
        }
        if (!field) {
            file[multisample_at + at] = 0;
        }
    }
    return true;
}

} // namespace Vulkan
