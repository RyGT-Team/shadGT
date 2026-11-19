// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/game_title.h"
#include "common/types.h"

namespace VideoCore {

// TITLE-001: the defaults that differ between GT Sport and GT7, in one table. Each was set for
// one game by a fix measured on that game; a merge of the two renderers that kept one side's
// value would have changed the other game without a line of conflict to show for it. A switch
// read where the default is used still overrides it. A row leaves this table when one value is
// proven on both games.
struct TitleDefaults {
    // SHADGT_NATIVE_PREPARE: game job threads prepare the draws for the recorder. GT Sport:
    // on with the native renderer (PERF-316). GT7: off (FIX-229).
    bool prepared_draws;
    // FIX-229 (GT7): prepared draws are refused while the hull-shader LDS emulation is on
    // (FIX-227, on by default) unless SHADGT_NATIVE_PREPARE_FORCE=1: with eight prepare workers
    // two of three Alsace runs lost the GPU device.
    bool prepared_draws_refused_with_lds;
    // SHADGT_NATIVE_CHUNK_MB: the recorder's upload chunk. GT Sport: 64 MB (PERF-GTS-407).
    // GT7: 1 MB (FIX-GT7-047: about 350 lists wait while a race loads, each holding a chunk).
    u32 native_chunk_mb;
    // SHADGT_DRIVER_CACHE_MAX_MB: the driver cache file's size limit. GT Sport: 4096
    // (PERF-GTS-407). GT7: 2048 (FIX-GT7-049: the driver keeps the whole file in memory).
    u32 driver_cache_max_mb;
    // FIX-SGT-010 (SHADGT_DRIVER_CACHE_ROTATE_MB): a driver cache file over this size is moved to
    // .old at start and the driver starts an empty one. The driver never drops pipelines, so
    // every renderer build that changes shader code adds a set the next builds never use, and
    // the driver keeps the whole file in memory: GT7's had grown to 2.96 GB (15.0 GiB resident
    // after 16 minutes; 11.8 GiB with a fresh cache, which was 229 MB after that session).
    // GT Sport: 2048. GT7: 1024.
    u32 driver_cache_rotate_mb;
    // GT7: the driver cache file also follows the pipeline cache setting.
    bool driver_cache_follows_pipeline_cache;
    // SHADGT_NATIVE_KEEP_TEXTURES_MB unset: idle textures kept up to an eighth of the device
    // budget (GT Sport, VRAM-KEEP-001) or half the video memory setting (GT7, FIX-GT7-030).
    bool kept_textures_from_vram_setting;
    // SHADGT_SO_SKIP unset, on NVIDIA: blended draws on shader objects (GT Sport, FIX-GTS-449)
    // or kept off them (GT7, SO-017). Other vendors keep them off for both games.
    bool so_blend_on_nvidia;
    // SHADGT_PREWARM unset: the pipelines stored by earlier sessions are built in the background
    // (PERF-038), where GPU.precompile_shaders allows it. GT Sport: yes. GT7: no. shadgt7 never
    // had it, and on GT7 it loses the GPU device at the title screen on every start with a
    // filled store (five of five, RTX 4070 only; the stored key of fs 0x3be222a4 with
    // vs 0x482206fa alone is enough), while the same starts pass with it off.
    bool stored_pipelines_prebuilt;
};

constexpr TitleDefaults TitleDefaultsOf(Common::GameTitle title) {
    switch (title) {
    case Common::GameTitle::Gt7:
        return {
            .prepared_draws = false,
            .prepared_draws_refused_with_lds = true,
            .native_chunk_mb = 1,
            .driver_cache_max_mb = 2048,
            .driver_cache_rotate_mb = 1024,
            .driver_cache_follows_pipeline_cache = true,
            .kept_textures_from_vram_setting = true,
            .so_blend_on_nvidia = false,
            .stored_pipelines_prebuilt = false,
        };
    case Common::GameTitle::GtSport:
    default:
        return {
            .prepared_draws = true,
            .prepared_draws_refused_with_lds = false,
            .native_chunk_mb = 64,
            .driver_cache_max_mb = 4096,
            .driver_cache_rotate_mb = 2048,
            .driver_cache_follows_pipeline_cache = false,
            .kept_textures_from_vram_setting = false,
            .so_blend_on_nvidia = true,
            .stored_pipelines_prebuilt = true,
        };
    }
}

inline const TitleDefaults& CurrentTitleDefaults() {
    static const TitleDefaults defaults = TitleDefaultsOf(Common::CurrentGameTitle());
    return defaults;
}

} // namespace VideoCore
