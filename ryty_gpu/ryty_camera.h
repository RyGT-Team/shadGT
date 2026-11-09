#pragma once
#include <cstdint>
#include <string>
#include "ryty_gpu/ryty_camera_settings.h"
namespace Ryty::Camera {
void DrawOverlay();
void ToggleOverlay();
void ResetOverlayLayout();
void CloseOverlay();
bool OverlayOpen();
void PublishStick(float x, float y, bool focused);
void RequestView(View view);
Store& Presets();
void InitializeRuntime();
std::uint32_t CurrentCar();
View CurrentView();
std::string CarName(std::uint32_t id);
std::string RuntimeStatus();
std::string RuntimeDiagnostics();
float NativeFov(View view);
float MinimumHeight(View view);
} // namespace Ryty::Camera
