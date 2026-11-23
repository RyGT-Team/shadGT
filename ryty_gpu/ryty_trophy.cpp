// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ryty_gpu/ryty_trophy.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <string_view>
#include <system_error>
#include <fmt/format.h>
#include "common/game_title.h"
#include "common/logging/log.h"
#include "common/path_util.h"

namespace Ryty {
namespace {

constexpr s64 RytyFirstUserId = 0x10000000;
constexpr s64 ShadFirstUserId = 1000;
constexpr s64 MaxSlots = 4;

std::string U8(const std::filesystem::path& path) {
    return Common::FS::PathToUTF8String(path);
}

bool LooksDecrypted(const std::filesystem::path& tropconf) {
    std::ifstream file{tropconf, std::ios::binary};
    std::string head(4096, '\0');
    file.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(file.gcount()));
    std::string_view text(head);
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    for (;;) {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
            text.remove_prefix(1);
        }
        std::string_view close;
        if (text.starts_with("<?")) {
            close = "?>";
        } else if (text.starts_with("<!--")) {
            close = "-->";
        } else {
            break;
        }
        const auto end = text.find(close);
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + close.size());
    }
    if (text.starts_with("<trophyconf")) {
        return true;
    }
    return std::string_view(head).find("<trophyconf") != std::string_view::npos;
}

std::mutex g_mutex;
std::set<std::string> g_prepared;

} // namespace

bool IsInsideDir(const std::filesystem::path& base, const std::filesystem::path& target) {
    std::error_code error;
    const auto base_full = std::filesystem::weakly_canonical(base, error);
    if (error) {
        return false;
    }
    const auto target_full = std::filesystem::weakly_canonical(target, error);
    if (error) {
        return false;
    }
    const auto relative = target_full.lexically_relative(base_full);
    if (relative.empty() || relative == ".") {
        return false;
    }
    const auto first = *relative.begin();
    return first != ".." && !relative.is_absolute();
}

s64 TrophyUserFolder(s64 user_id) {
    if (user_id >= RytyFirstUserId && user_id < RytyFirstUserId + MaxSlots) {
        return ShadFirstUserId + (user_id - RytyFirstUserId);
    }
    return user_id;
}

std::filesystem::path TrophyProgressFile(s64 user_id, const std::string& np_comm_id,
                                         const std::filesystem::path& legacy) {
    if (const char* env = std::getenv("RYGT_TROPHY_PROGRESS_SD"); env && env[0] == '0') {
        return legacy;
    }
    const s64 folder = TrophyUserFolder(user_id);
    const auto root = std::filesystem::current_path();
    auto dir = root / "user" / "savedata" / std::string(Common::CurrentGameSerial()) / "_sd" /
               "trophies_progress";
    auto old_dir = root / "_sd" / "trophies_progress";
    if (folder != 1000) {
        dir /= std::to_string(folder);
        old_dir /= std::to_string(folder);
    }
    const auto file = dir / (np_comm_id + ".xml");
    const auto old_file = old_dir / (np_comm_id + ".xml");
    std::error_code error;
    if (!std::filesystem::exists(file, error) &&
        std::filesystem::is_regular_file(old_file, error)) {
        std::filesystem::create_directories(dir, error);
        std::error_code copy_error;
        std::filesystem::copy_file(old_file, file, copy_error);
        if (copy_error) {
            LOG_ERROR(Lib_NpTrophy, "FEAT-GT7-189 fail: could not copy {} to {}: {}",
                      U8(old_file), U8(file), copy_error.message());
        } else {
            LOG_INFO(Lib_NpTrophy, "FEAT-GT7-189 ok: trophy progress copied from {} to {}",
                     U8(old_file), U8(file));
        }
    }
    return file;
}

void PrepareTrophies(const std::string& np_comm_id, u32 service_label,
                     const std::filesystem::path& extract_dir,
                     const std::filesystem::path& user_xml,
                     const std::filesystem::path& legacy_xml) {
    std::scoped_lock lock{g_mutex};
    std::error_code error;
    const auto tropconf = extract_dir / "Xml" / "TROPCONF.XML";

    if (!IsValidNpCommId(np_comm_id) ||
        !IsInsideDir(Common::FS::GetUserPath(Common::FS::PathType::TrophyDir), extract_dir)) {
        LOG_ERROR(Lib_NpTrophy,
                  "FEAT-GT7-185 fail: trophy folder {} is not inside the trophy directory; "
                  "trophies off for this set",
                  U8(extract_dir));
        return;
    }

    // FEAT-GT7-185: trophies only work from files the player already extracted into
    // user\trophy\<npCommId>; RyGT does not decrypt the game's trophy files.
    (void)service_label;
    if (!g_prepared.contains(np_comm_id)) {
        g_prepared.insert(np_comm_id);
        if (!std::filesystem::is_regular_file(tropconf, error)) {
            LOG_WARNING(Lib_NpTrophy,
                        "FEAT-GT7-185: trophy files for {} are not there (no {}); trophies off. "
                        "Put the extracted trophy files in {}",
                        np_comm_id, U8(tropconf), U8(extract_dir));
        } else if (!LooksDecrypted(tropconf)) {
            LOG_WARNING(Lib_NpTrophy,
                        "FEAT-GT7-185: {} is not a plain <trophyconf> file (still encrypted?); "
                        "trophies off",
                        U8(tropconf));
        } else {
            LOG_INFO(Lib_NpTrophy, "FEAT-GT7-185: trophy files for {} found in {}", np_comm_id,
                     U8(extract_dir));
        }
    }
    if (!std::filesystem::is_regular_file(tropconf, error) || !LooksDecrypted(tropconf)) {
        return;
    }

    if (std::filesystem::exists(user_xml, error)) {
        return;
    }
    if (legacy_xml != user_xml && std::filesystem::is_regular_file(legacy_xml, error)) {
        std::filesystem::create_directories(user_xml.parent_path(), error);
        std::error_code copy_error;
        std::filesystem::copy_file(legacy_xml, user_xml, copy_error);
        if (!copy_error) {
            LOG_INFO(Lib_NpTrophy, "FEAT-GT7-189 ok: trophy progress copied from {} to {}",
                     U8(legacy_xml), U8(user_xml));
            return;
        }
        LOG_ERROR(Lib_NpTrophy, "FEAT-GT7-189 fail: could not copy {} to {}: {}", U8(legacy_xml),
                  U8(user_xml), copy_error.message());
    }
    auto source = extract_dir / "Xml" / "TROP.XML";
    if (!std::filesystem::is_regular_file(source, error)) {
        source = tropconf;
    }
    if (!std::filesystem::is_regular_file(source, error)) {
        return;
    }
    std::filesystem::create_directories(user_xml.parent_path(), error);
    std::error_code copy_error;
    std::filesystem::copy_file(source, user_xml, copy_error);
    if (copy_error) {
        LOG_ERROR(Lib_NpTrophy, "FEAT-GT7-185 fail: could not create {}: {}", U8(user_xml),
                  copy_error.message());
    } else {
        LOG_INFO(Lib_NpTrophy, "FEAT-GT7-185 ok: unlock file {} created from {}", U8(user_xml),
                 U8(source.filename()));
    }
}

} // namespace Ryty
