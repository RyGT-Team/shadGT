#pragma once
#include <SDL3/SDL_events.h>
#include <algorithm>
#include <cmath>

namespace Ryty::Camera {
inline float NormalizeOrbitAxis(int raw) {
    const float value = std::clamp((raw - 128) / 127.f, -1.f, 1.f);
    const float magnitude = std::abs(value);
    constexpr float deadzone = .12f;
    if (magnitude <= deadzone) return 0;
    return std::copysign((magnitude - deadzone) / (1.f - deadzone), value);
}
enum class InputRoute { Normal, InterfaceOnly, ReleaseBinding, Gamepad };
// SDL event routing only; never suppress joystick devices, hotplug or haptic output.
constexpr InputRoute RouteInput(bool open, Uint32 type) {
    if (!open) return InputRoute::Normal;
    switch (type) {
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return InputRoute::ReleaseBinding;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL:
        return InputRoute::InterfaceOnly;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        return InputRoute::Gamepad;
    default:
        return InputRoute::Normal;
    }
}
} // namespace Ryty::Camera
