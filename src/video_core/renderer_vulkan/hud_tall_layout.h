// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// FEAT-SGT-209: GT Sport's race HUD on a screen taller than 16:9 (FEAT-SGT-203): its top pieces
// move up to the screen's top edge and its bottom row down to the bottom edge, at their 16:9
// size; what is between them (the standings, the messages) stays in the 16:9 band in the middle.
// The same result as GT7's FEAT-SGT-201 (shadGT #152), through GT Sport's HUD path. Kept free of
// Vulkan for the unit tests. Fractions are of the 16:9 HUD (0..1 both ways).
//
// Measured on GT Sport's race HUD at 1920x1080 (2026-11-27): position and laps x 0.02..0.20,
// y 0.03..0.11; the standings x 0.02..0.15 from y 0.16; the lap timer y 0.226..0.259 in the
// middle; the track map x 0.77..0.98, y 0.06..0.24, and under it the lap times from y 0.275 (a
// row a lap, growing down); the bottom row (lap time, gauges, course map) from y 0.76. So the
// top edge's pieces end at 0.13 in the left column and at 0.265 elsewhere: the standings and the
// lap times stay whole in the middle band. The bottom row starts at 0.70 everywhere.
//
// FIX-SGT-240, from the owner's captures at 1920x1440 (2026-11-27, RTX 4070). A cut must lie
// between two elements, never inside one: an element moves whole (Move, with its slack) while
// the copies between the HUD's picture and the game's display go by these regions, so the rows
// of a moved element past its region's cut were read from the middle band's place.
//  - The track map's box reaches 0.2685 (290 lines of 1080), past the 0.265 cut: four lines of
//    the scene from 180 lines lower showed as a dark rule under the map, as wide as its box.
//    The right column's top pieces now end at 0.272, between the map and the lap times (0.275).
//  - The bumper and bonnet views' dashboard starts higher than the chase view's row: AUTO
//    DRIVE at 0.623, the shift lights at 0.656, the rev bar and the speed's panel at 0.667. With
//    the cut at 0.70 the digits and gauges went down and the panel, the rev bar and AUTO DRIVE
//    stayed 180 lines above them. The middle column's bottom row now starts at 0.617, under the
//    start countdown's ring (which ends at 0.610).

#include <array>

namespace Vulkan::HudTall {

constexpr float LeftColumn = 0.30f;  // x below it: the left column
constexpr float RightColumn = 0.70f; // x above it: the right column
constexpr float LeftTop = 0.13f;     // the left column's top pieces end there
constexpr float OtherTop = 0.265f;   // the middle column's top pieces end there
constexpr float RightTop = 0.272f;   // FIX-SGT-240: the right column's, under the track map's box
constexpr float Bottom = 0.70f;      // the bottom row starts there in the left and right columns
constexpr float MiddleBottom = 0.617f; // FIX-SGT-240: in the middle column (the large dashboard)

/// Which column (-1, 0, 1) a horizontal position belongs to.
constexpr int Column(float x) {
    return x < LeftColumn ? -1 : x >= RightColumn ? 1 : 0;
}

/// Where the top pieces of that column end.
constexpr float TopEnd(int column) {
    return column < 0 ? LeftTop : column == 0 ? OtherTop : RightTop;
}

/// Where the bottom row of that column starts.
constexpr float BottomStart(int column) {
    return column == 0 ? MiddleBottom : Bottom;
}

/// Where an element of the HUD (its box) goes: -1 up to the top edge, 1 down to the bottom edge,
/// 0 it stays. Its column is that of its centre; it moves only when it lies wholly in the top
/// pieces' part of that column or in the bottom row (`slack` lets a box reach a little past).
constexpr int Move(float x0, float x1, float y0, float y1, float slack = 0.005f) {
    const int column = Column((x0 + x1) * 0.5f);
    if (y1 <= TopEnd(column) + slack) {
        return -1;
    }
    if (y0 >= BottomStart(column) - slack) {
        return 1;
    }
    return 0;
}

/// A part of the HUD that moves as one, for draws over the whole HUD (cut into these parts)
/// and for the copy back to the game's display buffer.
struct Region {
    float u0, v0, u1, v1;
    int column;      // -1, 0, 1
    int move;        // -1 up, 0 stays, 1 down
    // The middle band and the bottom row's top (from the top pieces to 0.80). Not 20..80% as in
    // WideHud's wide zones: on a taller screen WideHud takes no "bar" verdict from these counts.
    bool middle_band;
};

/// Three columns, four rows each: the top pieces, the middle band, then the bottom row cut at
/// 0.80 as WideHud's zones are (for its counts of whole-HUD draws).
constexpr std::array<Region, 12> Regions = [] {
    std::array<Region, 12> regions{};
    constexpr float cols[4]{0.0f, LeftColumn, RightColumn, 1.0f};
    for (int c = 0; c < 3; ++c) {
        const float rows[5]{0.0f, TopEnd(c - 1), BottomStart(c - 1), 0.80f, 1.0f};
        for (int r = 0; r < 4; ++r) {
            regions[size_t(c * 4 + r)] = Region{cols[c], rows[r], cols[c + 1], rows[r + 1], c - 1,
                                                r == 0 ? -1 : r == 1 ? 0 : 1, r == 1 || r == 2};
        }
    }
    return regions;
}();

} // namespace Vulkan::HudTall
