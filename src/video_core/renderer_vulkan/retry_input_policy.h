// Copyright 2026 RyGT team. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
namespace Vulkan {
inline bool DirectOnlyRetryInputs(bool walker, bool fetch_shader, bool translated_fetch) {
    return !walker && !fetch_shader && !translated_fetch;
}
inline bool DirectOnlyRetryLayout(bool plain_vertex_pipeline) {
    return plain_vertex_pipeline;
}
inline bool OwnedRetryJobMatches(std::uint32_t stage, std::uintptr_t base,
                                 std::span<const std::uint32_t> user_data,
                                 std::span<const std::uint32_t> code,
                                 std::uint32_t job_stage, std::uintptr_t job_base,
                                 std::span<const std::uint32_t> job_user_data,
                                 std::span<const std::uint32_t> job_code, bool direct_only) {
    return direct_only && stage == job_stage && base == job_base &&
           std::ranges::equal(user_data, job_user_data) && std::ranges::equal(code, job_code);
}
} // namespace Vulkan
