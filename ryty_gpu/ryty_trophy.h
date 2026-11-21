// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include "common/types.h"

namespace Ryty {

s64 TrophyUserFolder(s64 user_id);

std::filesystem::path TrophyProgressFile(s64 user_id, const std::string& np_comm_id,
                                         const std::filesystem::path& legacy);

void PrepareTrophies(const std::string& np_comm_id, u32 service_label,
                     const std::filesystem::path& extract_dir,
                     const std::filesystem::path& user_xml,
                     const std::filesystem::path& legacy_xml);

} // namespace Ryty
