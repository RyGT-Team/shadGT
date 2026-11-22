// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include "common/game_title.h"
#include "video_core/renderer_vulkan/hdr_rolloff_policy.h"
#include "video_core/renderer_vulkan/native_chunk_policy.h"
#include "video_core/title_defaults.h"

using Common::GameTitle;
using Common::GameTitleOf;

// GAME-001: the Ryty build reports GT Sport's title id for both games, so there the executable's
// name decides.
static_assert(GameTitleOf(L"GT7.exe", "CUSA03220", "Gran Turismo Sport") == GameTitle::Gt7);
static_assert(GameTitleOf(L"gt7.EXE", "CUSA03220", "Gran Turismo Sport") == GameTitle::Gt7);
static_assert(GameTitleOf(L"GTSport.exe", "CUSA03220", "Gran Turismo Sport") ==
              GameTitle::GtSport);
// A name that only contains GT7.exe is not GT7's.
static_assert(GameTitleOf(L"NotGT7.exe", "CUSA03220", "Gran Turismo Sport") == GameTitle::GtSport);
static_assert(GameTitleOf(L"GT7.exe.bak", "CUSA03220", "Gran Turismo Sport") ==
              GameTitle::GtSport);
// The standalone emulator reads the real title: both of GT7's regions, other regions by name.
static_assert(GameTitleOf(L"shadps4.exe", "CUSA24767", "") == GameTitle::Gt7);
static_assert(GameTitleOf(L"shadps4.exe", "CUSA24769", "") == GameTitle::Gt7);
static_assert(GameTitleOf(L"shadps4.exe", "PCJS00000", "Gran Turismo\xC2\xAE 7") == GameTitle::Gt7);
static_assert(GameTitleOf(L"shadps4.exe", "CUSA03220", "Gran Turismo Sport") ==
              GameTitle::GtSport);
static_assert(GameTitleOf(L"", "", "") == GameTitle::GtSport);

// TITLE-001: each game keeps the defaults its own renderer had before the two were merged.
static_assert(VideoCore::TitleDefaultsOf(GameTitle::GtSport).prepared_draws);
static_assert(!VideoCore::TitleDefaultsOf(GameTitle::GtSport).prepared_draws_refused_with_lds);
static_assert(VideoCore::TitleDefaultsOf(GameTitle::GtSport).native_chunk_mb == 64);
static_assert(VideoCore::TitleDefaultsOf(GameTitle::GtSport).driver_cache_max_mb == 4096);
static_assert(!VideoCore::TitleDefaultsOf(GameTitle::GtSport).driver_cache_follows_pipeline_cache);
static_assert(!VideoCore::TitleDefaultsOf(GameTitle::GtSport).kept_textures_from_vram_setting);
static_assert(VideoCore::TitleDefaultsOf(GameTitle::GtSport).so_blend_on_nvidia);
static_assert(!VideoCore::TitleDefaultsOf(GameTitle::Gt7).prepared_draws);
static_assert(VideoCore::TitleDefaultsOf(GameTitle::Gt7).prepared_draws_refused_with_lds);
static_assert(VideoCore::TitleDefaultsOf(GameTitle::Gt7).native_chunk_mb == 1);
static_assert(VideoCore::TitleDefaultsOf(GameTitle::Gt7).driver_cache_max_mb == 2048);
static_assert(VideoCore::TitleDefaultsOf(GameTitle::Gt7).driver_cache_rotate_mb == 1024); // FIX-SGT-010
static_assert(VideoCore::TitleDefaultsOf(GameTitle::GtSport).driver_cache_rotate_mb == 2048);
static_assert(VideoCore::TitleDefaultsOf(GameTitle::Gt7).driver_cache_follows_pipeline_cache);
static_assert(VideoCore::TitleDefaultsOf(GameTitle::Gt7).kept_textures_from_vram_setting);
static_assert(!VideoCore::TitleDefaultsOf(GameTitle::Gt7).so_blend_on_nvidia);
// FIX-SGT-113: both games tone map to the display's peak themselves, so no roll-off by default.
static_assert(Vulkan::HdrRolloffDefaultNits == 0.0f);

TEST(TitleDefaults, HdrRolloffIsOffUnlessAskedFor) {
    constexpr float off = Vulkan::HdrRolloffDefaultNits;
    EXPECT_EQ(Vulkan::HdrRolloffNits(nullptr, off), 0.0f);
    EXPECT_EQ(Vulkan::HdrRolloffNits(nullptr, 1280.0f), 1280.0f); // the settings file's value
    // The switch overrides both ways, and only with a number in range.
    EXPECT_EQ(Vulkan::HdrRolloffNits("1000", off), 1000.0f); // GT Sport's picture before
    EXPECT_EQ(Vulkan::HdrRolloffNits("0", 1280.0f), 0.0f);
    EXPECT_EQ(Vulkan::HdrRolloffNits("10000", off), 10000.0f);
    for (const char* refused : {"", "-1", "10001", "nan", "inf", "800x", "off"}) {
        EXPECT_EQ(Vulkan::HdrRolloffNits(refused, off), 0.0f) << refused;
        EXPECT_EQ(Vulkan::HdrRolloffNits(refused, 1280.0f), 1280.0f) << refused;
    }
}

TEST(TitleDefaults, ChunkSizeWithoutTheSwitchIsTheTitles) {
    constexpr std::uint64_t mb = 1024 * 1024;
    const auto gt7 = VideoCore::TitleDefaultsOf(GameTitle::Gt7).native_chunk_mb;
    const auto sport = VideoCore::TitleDefaultsOf(GameTitle::GtSport).native_chunk_mb;
    EXPECT_EQ(Vulkan::NativeChunkBytes(nullptr, gt7), 1 * mb);
    EXPECT_EQ(Vulkan::NativeChunkBytes("", gt7), 1 * mb);
    EXPECT_EQ(Vulkan::NativeChunkBytes(nullptr, sport), 64 * mb);
    // The switch overrides the title's default, for either game.
    EXPECT_EQ(Vulkan::NativeChunkBytes("8", gt7), 8 * mb);
    EXPECT_EQ(Vulkan::NativeChunkBytes("8", sport), 8 * mb);
}
