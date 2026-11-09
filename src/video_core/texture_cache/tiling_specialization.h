// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <bit>
#include "video_core/texture_cache/image_info.h"

namespace VideoCore {

inline u64 TilingPipelineKey(const ImageInfo& info, bool is_tiler) {
    return (u64(info.alt_tile) << 49) | (u64(is_tiler) << 48) | (u64(info.tile_mode) << 40) |
           (u64(info.num_bits) << 8) | info.num_samples;
}

inline std::array<u32, 12> TilingSpecialization(const ImageInfo& info, bool is_tiler) {
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    const bool is_macro = AmdGpu::IsMacroTiled(info.array_mode);
    std::array<u32, 12> spec_data{
        info.num_samples,
        u32(micro_tile_mode),
        AmdGpu::GetMicroTileThickness(info.array_mode),
        u32(is_tiler),
    };
    if (is_macro) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        spec_data[4] = u32(info.array_mode);
        spec_data[5] = u32(info.alt_tile ? AmdGpu::GetAltPipeConfig(info.tile_mode)
                                         : AmdGpu::GetPipeConfig(info.tile_mode));
        spec_data[6] = AmdGpu::GetBankWidth(macro_tile_mode);
        spec_data[7] = info.alt_tile ? AmdGpu::GetAltBankHeight(macro_tile_mode)
                                     : AmdGpu::GetBankHeight(macro_tile_mode);
        spec_data[8] = info.alt_tile ? AmdGpu::GetAltNumBanks(macro_tile_mode)
                                     : AmdGpu::GetNumBanks(macro_tile_mode);
        spec_data[9] = std::bit_width(spec_data[8]) - 1;
        spec_data[10] = AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode, micro_tile_mode,
                                                   info.num_bits);
        spec_data[11] = info.alt_tile ? AmdGpu::GetAltMacrotileAspect(macro_tile_mode)
                                      : AmdGpu::GetMacrotileAspect(macro_tile_mode);
    }
    return spec_data;
}

} // namespace VideoCore
