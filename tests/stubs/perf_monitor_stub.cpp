// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <string>
#include "common/types.h"

namespace Common {
// Preserve the inspection tools' performance-switch behavior in standalone tests.
bool PerfFeatureEnabled(u32 id) {
    const char* env = std::getenv("SHADGT_DISABLE_PERF");
    if (!env) return true;
    const std::string list = std::string(",") + env + ",";
    return list.find("," + std::to_string(id) + ",") == std::string::npos;
}
}
