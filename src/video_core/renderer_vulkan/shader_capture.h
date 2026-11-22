// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <span>

#include "common/types.h"

// FEAT-SGT-117: a preparation run (SHADGT_PREPARE_SHADERS=1) copies, from the game's memory, the code
// of every shader program the manifest has a record for and this PC has no code of, into
// <serial>-programs/<hash>.bin. The start-up pre-translation of the next launches reads it there
// (cut to the record's length and checked against its XXH3), so the records can be translated before
// the game draws them. The code stays on this PC. Without the switch nothing runs.
namespace Vulkan::ShaderCapture {

/// SHADGT_PREPARE_SHADERS=1 (cached). False in builds where the capture can't run (not Ryty on Windows).
bool Enabled();

/// Starts the capture thread when Enabled(): it reads the manifest's records, leaves out the programs
/// whose code is already in `programs_dir` or `capture_dir`, then scans the game's memory now and then.
/// Called once, by the pipeline cache, once its folders are known and merged.
void Start(const std::filesystem::path& programs_dir, const std::filesystem::path& manifest_dir,
           const std::filesystem::path& capture_dir);

/// For a preparation that loads things one after the other: has the capture thread scan now and waits
/// for that scan, as long as the last scans took (5 s at least). Returns whether it was scanned; at
/// once (false) when the capture is off. Never scans during a race.
bool ScanNowAndWait();

/// A program's code starts 256-byte aligned with s_mov_b32 vcc_hi (0xBEEB03FF), whose operand puts its
/// footer at start + (operand + 1) * 8 bytes (AmdGpu::SearchBinaryInfo). `block` holds the words from
/// `low` (256-byte aligned) to at least 8 bytes past the last 256-byte boundary at or below `footer`.
/// Returns the start nearest below the footer that points at it, if any.
inline std::optional<u64> FindProgramStart(std::span<const u32> block, u64 low, u64 footer) {
    constexpr u32 Token = 0xBEEB03FF;
    if (footer < low || (low & 0xff) != 0) {
        return std::nullopt;
    }
    for (u64 at = footer & ~u64{0xff};; at -= 0x100) {
        const size_t i = size_t(at - low) / 4;
        if (i + 1 < block.size() && block[i] == Token && at + (u64{block[i + 1]} + 1) * 8 == footer) {
            return at;
        }
        if (at < low + 0x100) {
            return std::nullopt;
        }
    }
}

} // namespace Vulkan::ShaderCapture
