// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// FEAT-SGT-207: GT Sport's taller race view on a screen taller than 16:9, kept free of the game
// for the unit tests.

#include <cmath>

namespace Ryty::Camera {

/// The vertical field of view (degrees) that keeps a camera's 16:9 width on a screen of `aspect`
/// (width over height) narrower than 16:9: the view grows in height instead of losing its sides.
/// GT Sport's cameras give a vertical angle (each driving camera writes it to view[22] every
/// frame; its width is that angle's tangent times the aspect). Anything else, or an angle that
/// would not stay below 179 degrees, is returned as it is.
inline float TallerViewFov(float fov, float aspect) {
    constexpr double wide = 16.0 / 9.0;
    if (!(fov > 0.0f && fov < 179.0f) || !(aspect > 0.25f) || aspect >= wide - 0.01) {
        return fov;
    }
    constexpr double pi = 3.14159265358979323846;
    const double half = std::tan(double(fov) * pi / 360.0) * wide / double(aspect);
    const double taller = std::atan(half) * 360.0 / pi;
    return taller < 179.0 ? float(taller) : fov;
}

} // namespace Ryty::Camera
