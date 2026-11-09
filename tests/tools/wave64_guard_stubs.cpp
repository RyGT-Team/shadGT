// SPDX-License-Identifier: GPL-2.0-or-later
// The standalone IR fixture deliberately has no emulator process or logging backend.
// Keep assertions fatal and errors visible; only logging infrastructure is stubbed.
#include <cstdio>
#include <cstdlib>
#include "common/logging/log.h"

namespace Common::Log {
std::array<Level, NUM_LOG_CLASSES> g_class_levels{};
void VLog(Class, Level level, const char*, int, const char*, fmt::string_view text,
          fmt::format_args args) {
    if (level >= Level::Error) {
        fmt::vprint(stderr, text, args);
        std::fputc('\n', stderr);
    }
}
} // namespace Common::Log

void assert_fail_impl() {
    std::fputs("wave64 guard regression assertion failed\n", stderr);
    std::abort();
}
[[noreturn]] void unreachable_impl() {
    std::fputs("wave64 guard regression reached unreachable code\n", stderr);
    std::abort();
}
