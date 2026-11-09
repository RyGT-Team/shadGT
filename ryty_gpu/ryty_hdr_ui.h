#pragma once

// HDR-UI (ryGT): GT Sport draws its HUD and menus into the HDR10 display buffer with SDR colours
// (no PQ encoding in its UI shaders), so their white came out at 10000 nits and colours clipped.
// Draws that blend into a display buffer while the game outputs HDR get a PQ encoding epilogue
// (sRGB to linear, the UI white in nits, BT.709 to BT.2020, PQ) through the pipeline key.

#include <array>
#include <atomic>

#include "common/types.h"

namespace Ryty {

inline std::atomic<bool> g_hdr_output{false};
/// UI-HIRES has a proxy (and so a UI layer) for the display buffers.
inline std::atomic<bool> g_ui_layer_ready{false};
inline std::array<std::atomic<u64>, 16> g_display_addresses{};

inline void AddDisplayAddress(u64 address) {
    if (address == 0) {
        return;
    }
    for (auto& known : g_display_addresses) {
        u64 seen = known.load(std::memory_order_relaxed);
        if (seen == address) {
            return;
        }
        if (seen == 0 && known.compare_exchange_strong(seen, address)) {
            return;
        }
    }
}

inline bool IsDisplayAddress(u64 address) {
    for (const auto& known : g_display_addresses) {
        if (known.load(std::memory_order_relaxed) == address) {
            return true;
        }
    }
    return false;
}

} // namespace Ryty
