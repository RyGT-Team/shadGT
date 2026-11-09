// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/spirv_check.h"

namespace Vulkan {
inline bool WriteComputeWarmRecipeExclusive(const std::filesystem::path& file, std::span<const u32> words) {
    static std::atomic<u64> next_temp{};
    std::filesystem::path dir;
    for (u32 attempt=0; attempt<1024; ++attempt) {
        dir=file; dir += ".tmp." + std::to_string(next_temp.fetch_add(1, std::memory_order_relaxed));
        std::error_code ec;
        if (std::filesystem::create_directory(dir,ec)) break;
        dir.clear();
        if (ec && ec != std::errc::file_exists) return false;
    }
    if (dir.empty()) return false;
    const auto temp=dir/"recipe";
    bool written=false;
    {
        std::ofstream out{temp,std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(words.data()),std::streamsize(words.size_bytes()));
        out.close(); written=bool(out);
    }
    std::error_code ec;
    if (written) std::filesystem::create_hard_link(temp,file,ec);
    else ec=std::make_error_code(std::errc::io_error);
    const bool saved=!ec;
    std::error_code ignored;
    std::filesystem::remove(temp,ignored);
    std::filesystem::remove(dir,ignored);
    return saved;
}
inline std::vector<std::filesystem::path> ListComputeWarmRecipes(
    const std::filesystem::path& directory, std::string_view policy_suffix) {
    std::error_code ec;
    std::vector<std::pair<std::uintmax_t, std::filesystem::path>> sized;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        if (entry.path().extension() != ".bin" || !entry.path().stem().string().ends_with(policy_suffix)) continue;
        std::error_code size_error;
        const auto size = entry.file_size(size_error);
        if (!size_error && sized.size() < 1024) sized.emplace_back(size, entry.path());
    }
    std::ranges::sort(sized, std::greater{}, &decltype(sized)::value_type::first);
    std::vector<std::filesystem::path> paths;
    paths.reserve(sized.size());
    for (auto& [size, path] : sized) paths.push_back(std::move(path));
    return paths;
}
// CPU-only validation before any recipe data is passed to Vulkan.
inline bool ValidateComputeWarmRecipe(std::span<const u32> words, u64 signature,
                                      u32 max_push, u32 max_spirv,
                                      bool subgroup32, bool subgroup64) {
    constexpr u32 Magic = 0x43574752, Version = 2;
    if (words.size() < 9 || words[0] != Magic || words[1] != Version ||
        (u64(words[3]) << 32 | words[2]) != signature || words[4] > 1 || words[5] > 1)
        return false;
    if (words[5] && !((subgroup32 && words[6] == 32) || (subgroup64 && words[6] == 64)))
        return false;
    const u32 count = words[7];
    size_t at = 8;
    if (count > 256 || words.size() < at + size_t(count) * 3 + 1) return false;
    u64 next = 0;
    for (u32 i = 0; i < count; ++i, at += 3) {
        // Vulkan descriptor types: sampler 0, sampled image 2, storage image 3, storage buffer 7.
        const u32 type = words[at + 1], n = words[at + 2];
        if (words[at] != next || !n || n > 256 ||
            (type != 0 && type != 2 && type != 3 && type != 7)) return false;
        next += n;
        if (next > 256) return false;
    }
    if (words[4] && next >= max_push) return false;
    const u32 n = words[at++];
    if (!n || words.size() - at != n) return false;
    const auto spirv = words.subspan(at, n);
    return IsCompleteSpirv(spirv) && spirv[1] <= max_spirv;
}
}
