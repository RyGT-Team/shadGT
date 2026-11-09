#pragma once

namespace Native {
constexpr bool MenuCompileMustWait(unsigned mode, bool in_race, bool capture_hooks,
                                   bool capture_idle) {
    if (capture_hooks && !capture_idle) return true;
    return !in_race && (mode != 2 || !capture_hooks);
}
} // namespace Native
