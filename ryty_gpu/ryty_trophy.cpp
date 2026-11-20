// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ryty_gpu/ryty_trophy.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string_view>
#include <system_error>
#include <fmt/format.h>
#include "common/game_title.h"
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/file_format/trp.h"
#include "ryty_gpu/ryty_app0.h"

namespace Ryty {
namespace {

constexpr s64 RytyFirstUserId = 0x10000000;
constexpr s64 ShadFirstUserId = 1000;
constexpr s64 MaxSlots = 4;

std::string U8(const std::filesystem::path& path) {
    return Common::FS::PathToUTF8String(path);
}

std::vector<u8> ReadTrophyKey() {
    std::filesystem::path key_path;
#ifdef _WIN32
    if (const wchar_t* value = _wgetenv(L"RYGT_TROPHY_KEY_FILE"); value && *value) {
        key_path = value;
    }
#else
    if (const char* value = std::getenv("RYGT_TROPHY_KEY_FILE"); value && *value) {
        key_path = value;
    }
#endif
    if (key_path.empty()) {
        LOG_INFO(Lib_NpTrophy, "FEAT-GT7-185: no trophy key (RYGT_TROPHY_KEY_FILE not set); "
                               "trophy files are not extracted");
        return {};
    }
    std::string text;
    if (std::ifstream file{key_path, std::ios::binary}) {
        text.assign(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    }
    std::string hex;
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            hex.push_back(c);
        }
    }
    const bool valid = hex.size() == 32 && std::all_of(hex.begin(), hex.end(), [](char c) {
                           return std::isxdigit(static_cast<unsigned char>(c)) != 0;
                       });
    std::vector<u8> key;
    if (valid) {
        key = KeyManager::HexStringToBytes(hex);
    }
    std::fill(text.begin(), text.end(), '\0');
    std::fill(hex.begin(), hex.end(), '\0');
    if (key.size() != 16) {
        LOG_WARNING(Lib_NpTrophy,
                    "FEAT-GT7-185: the trophy key file is missing or not 32 hex digits; "
                    "trophy files are not extracted");
        return {};
    }
    return key;
}

std::filesystem::path FindTrp(u32 service_label) {
    const auto wanted = fmt::format("trophy{:02}.trp", service_label);
    const auto root = std::filesystem::current_path();
    const App0Roots roots{Common::App0Folder(root), Common::RygtIniPath(root)};
    for (const auto& path : App0ListFiles(root, roots, std::filesystem::path("sce_sys") / "trophy")) {
        auto name = U8(path.filename());
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name == wanted) {
            return path;
        }
    }
    return {};
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
    auto dir = std::filesystem::current_path() / "_sd" / "trophies_progress";
    if (folder != 1000) {
        dir /= std::to_string(folder);
    }
    return dir / (np_comm_id + ".xml");
}

void PrepareTrophies(const std::string& np_comm_id, u32 service_label,
                     const std::filesystem::path& extract_dir,
                     const std::filesystem::path& user_xml,
                     const std::filesystem::path& legacy_xml) {
    std::scoped_lock lock{g_mutex};
    std::error_code error;
    const auto tropconf = extract_dir / "Xml" / "TROPCONF.XML";

    if (!g_prepared.contains(np_comm_id)) {
        g_prepared.insert(np_comm_id);
        if (std::filesystem::is_regular_file(tropconf, error)) {
            LOG_INFO(Lib_NpTrophy, "FEAT-GT7-185: {} already extracted in {}", np_comm_id,
                     U8(extract_dir));
        } else if (auto key = ReadTrophyKey(); !key.empty()) {
            const auto trp_path = FindTrp(service_label);
            if (trp_path.empty()) {
                LOG_ERROR(Lib_NpTrophy,
                          "FEAT-GT7-185 fail: no sce_sys/trophy/trophy{:02}.trp in the /app0 "
                          "folders",
                          service_label);
            } else {
                auto manager = KeyManager::GetInstance();
                const auto previous = manager->GetAllKeys();
                KeyManager::AllKeys keys = previous;
                keys.TrophyKeySet.ReleaseTrophyKey = std::move(key);
                manager->SetAllKeys(keys);
                std::fill(keys.TrophyKeySet.ReleaseTrophyKey.begin(),
                          keys.TrophyKeySet.ReleaseTrophyKey.end(), u8{0});
                TRP trp;
                const bool extracted = trp.Extract(trp_path, np_comm_id, extract_dir);
                manager->SetAllKeys(previous);
                if (!extracted) {
                    LOG_ERROR(Lib_NpTrophy, "FEAT-GT7-185 fail: could not extract {} into {}",
                              U8(trp_path), U8(extract_dir));
                } else if (!LooksDecrypted(tropconf)) {
                    LOG_ERROR(Lib_NpTrophy,
                              "FEAT-GT7-185 fail: {} is not <trophyconf> (wrong trophy key?); "
                              "removed",
                              U8(tropconf));
                    std::filesystem::remove_all(extract_dir, error);
                } else {
                    LOG_INFO(Lib_NpTrophy, "FEAT-GT7-185 ok: {} ({}) extracted into {}",
                             np_comm_id, U8(trp_path), U8(extract_dir));
                }
            }
        }
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
