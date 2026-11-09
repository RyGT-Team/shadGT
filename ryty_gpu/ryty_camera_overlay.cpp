#include "ryty_gpu/ryty_camera.h"
#include "ryty_gpu/ryty_input_bridge.h"
#include <atomic>
#include <imgui.h>
#include <imgui_internal.h>
namespace ImGui::Core {
bool IsGamepadInputCaptured();
} // namespace ImGui::Core
namespace Ryty::Camera {
namespace {
std::atomic<bool> open{}, reset_layout{};
ImVec4 RGB(int r, int g, int b, float a = 1) {
  return {r / 255.f, g / 255.f, b / 255.f, a};
}
bool EditSettings(Settings &s, View camera, int &selected, float scale) {
  bool changed = false;
  if (!ImGui::BeginTable("CameraColumns", 3,
                         ImGuiTableFlags_SizingStretchSame |
                             ImGuiTableFlags_BordersInnerV))
    return false;
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted("Camera");
  ImGui::SetNextItemWidth(-1);
  if (ImGui::Combo("##Camera", &selected, Names.data(),
                   static_cast<int>(Names.size())))
    RequestView(static_cast<View>(selected));
  // Do not mix the previous camera's values into a newly selected view this
  // frame.
  ImGui::BeginDisabled(camera != static_cast<View>(selected));
  const float native = NativeFov(camera);
  bool custom = s.fov != 0;
  ImGui::BeginDisabled(!custom && native <= 0);
  if (ImGui::Checkbox("Custom FOV", &custom)) {
    s.fov = custom ? std::clamp(native, 30.f, 100.f) : 0;
    changed = true;
  }
  ImGui::EndDisabled();
  ImGui::SetNextItemWidth(-75 * scale);
  if (custom)
    changed |= ImGui::SliderFloat("FOV", &s.fov, 30, 100, "%.1f deg");
  else {
    float shown = native;
    ImGui::BeginDisabled();
    ImGui::SliderFloat("FOV", &shown, 30, 100,
                       native > 0 ? "%.1f deg" : "Original");
    ImGui::EndDisabled();
  }
  const auto limit = Limits(camera);
  const float minimum = camera == View::Bumper
      ? std::clamp(MinimumHeight(camera), limit.height_min, limit.height_max)
      : limit.height_min;
  ImGui::SetNextItemWidth(-75 * scale);
  changed |= ImGui::SliderFloat("Height", &s.height,
                                minimum, limit.height_max, "%+.3f m", ImGuiSliderFlags_AlwaysClamp);
  if (camera != View::Bumper) {
    ImGui::SetNextItemWidth(-75 * scale);
    changed |= ImGui::SliderFloat("Distance", &s.distance, limit.distance_min,
                                  limit.distance_max, "%+.3f m", ImGuiSliderFlags_AlwaysClamp);
  }
  ImGui::TableSetColumnIndex(1);
  if (camera == View::Chase) {
    ImGui::TextUnformatted("Orbit");
    changed |= ImGui::Checkbox("Right stick orbit", &s.orbit);
    ImGui::BeginDisabled(!s.orbit);
    ImGui::SetNextItemWidth(-105 * scale);
    changed |=
        ImGui::SliderFloat("Sensitivity", &s.sensitivity, -15, 15, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("0 = default speed. Negative = slower. Positive = faster. Direction uses Invert X/Y only.");
    ImGui::SetNextItemWidth(-105 * scale);
    changed |=
        ImGui::SliderFloat("Smoothness", &s.smoothing_ms, 0, 1000, "%.0f ms");
    changed |= ImGui::Checkbox("Invert X", &s.invert_x);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Invert Y", &s.invert_y);
    ImGui::EndDisabled();
  } else {
    ImGui::TextUnformatted("Position");
    ImGui::TextWrapped("Height: + up / - down.");
    if (camera != View::Bumper)
      ImGui::TextWrapped("Distance: + forward / - backward.");
    ImGui::TextWrapped("Zero restores the original position.");
  }
  ImGui::TableSetColumnIndex(2);
  if (camera == View::Chase) {
    ImGui::TextUnformatted("Auto Center");
    ImGui::BeginDisabled(!s.orbit);
    changed |= ImGui::Checkbox("Smooth return", &s.auto_return);
    ImGui::BeginDisabled(!s.auto_return);
    ImGui::SetNextItemWidth(-105 * scale);
    changed |=
        ImGui::SliderFloat("Return delay", &s.return_seconds, 0, 10, "%.1f s", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::TextWrapped("Returns smoothly after input stops.");
  } else {
    ImGui::TextUnformatted("Original settings");
    ImGui::TextWrapped(
        "Restore Defaults returns this camera to the game's original values.");
    if (camera != View::Cockpit)
      ImGui::TextWrapped("Height is limited above the road.");
  }
  ImGui::EndDisabled();
  ImGui::EndTable();
  return changed;
}
} // namespace
bool OverlayOpen() { return open.load(std::memory_order_relaxed); }
void ToggleOverlay() { open.store(!OverlayOpen(), std::memory_order_relaxed); }
void ResetOverlayLayout() {
  reset_layout.store(true);
  open.store(true);
}
void CloseOverlay() { open.store(false, std::memory_order_relaxed); }
void DrawOverlay() {
  static bool was_visible = false, focus = false;
  static bool suppressing_gamepad_nav = false, saved_gamepad_nav = false;
  static int selected = 0;
  static View last_observed = View::Count;
  const bool visible = OverlayOpen();
  if (visible && !was_visible) {
    focus = true;
    last_observed = View::Count;
  }
  was_visible = visible;
  // Camera controls use mouse/keyboard only. Preserve native gamepad navigation
  // whenever another native window (including F3 Controllers) owns input capture.
  auto& io = ImGui::GetIO();
  const bool suppress = visible && !Ryty::ControllerOverlayOpen() &&
                        !ImGui::Core::IsGamepadInputCaptured();
  if (suppress != suppressing_gamepad_nav) {
    if (suppress) {
      saved_gamepad_nav = (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0;
      io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    } else if (saved_gamepad_nav) {
      io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    }
    suppressing_gamepad_nav = suppress;
  }
  ImGui::GetIO().MouseDrawCursor = visible || Ryty::ControllerOverlayOpen();
  if (!visible)
    return;
  auto &store = Presets();
    auto appearance = store.ReadAppearance();
  const auto *viewport = ImGui::GetMainViewport();
  const auto area = viewport->WorkSize, origin = viewport->WorkPos;
  if (area.x < 1 || area.y < 1)
    return;
  const float scale =
      std::clamp(std::min(area.y / 1000.f, area.x / 1600.f), .50f, 2.4f) *
      appearance.scale;
  const float inherited =
      ImGui::GetFontSize() / std::max(1.f, ImGui::GetStyle().FontSizeBase);
  ImGui::PushFont(nullptr,
                  appearance.font_size * scale / std::max(.01f, inherited));
  // Values and shared font match the existing Controllers overlay; no new font
  // atlas.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {18 * scale, 14 * scale});
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8 * scale, 5 * scale});
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * scale, 6 * scale});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3 * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 4 * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1);
  const auto &bg = appearance.background;
  const auto &ac = appearance.accent;
  const ImVec4 accent{ac[0], ac[1], ac[2], 1};
  const std::pair<ImGuiCol, ImVec4> colors[] = {
      {ImGuiCol_WindowBg, {bg[0], bg[1], bg[2], appearance.opacity}},
      {ImGuiCol_PopupBg, RGB(40, 40, 42, .99f)},
      {ImGuiCol_Border, RGB(70, 70, 74)},
      {ImGuiCol_Text, RGB(230, 230, 230)},
      {ImGuiCol_TextDisabled, RGB(150, 150, 150)},
      {ImGuiCol_FrameBg, RGB(45, 45, 48)},
      {ImGuiCol_FrameBgHovered, RGB(58, 58, 61)},
      {ImGuiCol_FrameBgActive, RGB(75, 75, 80)},
      {ImGuiCol_Button, RGB(58, 58, 61)},
      {ImGuiCol_ButtonHovered, RGB(75, 75, 80)},
      {ImGuiCol_ButtonActive, RGB(70, 70, 74)},
      {ImGuiCol_Header, {ac[0], ac[1], ac[2], .55f}},
      {ImGuiCol_HeaderHovered, RGB(206, 40, 52, .65f)},
      {ImGuiCol_HeaderActive, accent},
      {ImGuiCol_Separator, RGB(70, 70, 74)},
      {ImGuiCol_NavCursor, RGB(206, 40, 52)},
      {ImGuiCol_SliderGrab, accent},
      {ImGuiCol_SliderGrabActive, RGB(206, 40, 52)},
      {ImGuiCol_CheckMark, RGB(240, 96, 96)},
      {ImGuiCol_Tab, RGB(45, 45, 48)},
      {ImGuiCol_TabSelected, RGB(58, 58, 61)},
      {ImGuiCol_TabSelectedOverline, accent},
      {ImGuiCol_TabHovered, RGB(75, 75, 80)}};
  for (auto [id, color] : colors)
    ImGui::PushStyleColor(id, color);
  const char *title = "Camera Settings###RytyCameraSettings";
  const auto *previous = ImGui::FindWindowByName(title);
  const auto *saved = ImGui::FindWindowSettingsByID(ImHashStr(title));
  const bool reset = reset_layout.exchange(false) ||
                     (previous && previous->DockId) ||
                     (!previous && saved && saved->DockId);
  if (reset) {
    appearance.x = .10f;
    appearance.y = .04f;
    appearance.width = .80f;
    appearance.height = .34f;
    store.SetAppearance(appearance);
  }
  static ImVec2 last_area{};
  const bool resized = area.x != last_area.x || area.y != last_area.y;
  const ImVec2 minimum{std::min(area.x * .96f, 1050 * scale),
                       std::min(area.y * .9f, 315 * scale)};
  const ImVec2 size{
      std::clamp(area.x * appearance.width, minimum.x, area.x * .98f),
      std::clamp(area.y * appearance.height, minimum.y, area.y * .96f)};
  ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
  if (!previous || reset || resized) {
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(
        {origin.x + std::clamp(area.x * appearance.x, 0.f, area.x - size.x),
         origin.y + std::clamp(area.y * appearance.y, 0.f, area.y - size.y)},
        ImGuiCond_Always);
  } else {
    ImGui::SetNextWindowPos(
        {std::clamp(previous->Pos.x, origin.x,
                    origin.x + std::max(0.f, area.x - previous->Size.x)),
         std::clamp(previous->Pos.y, origin.y,
                    origin.y + std::max(0.f, area.y - previous->Size.y))},
        ImGuiCond_Always);
  }
  last_area = area;
  ImGui::SetNextWindowSizeConstraints(minimum, {area.x * .98f, area.y * .96f});
  if (focus) {
    ImGui::SetNextWindowFocus();
    focus = false;
  }
  bool show = true;
  if (ImGui::Begin(title, nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                       ImGuiWindowFlags_NoDocking |
                       ImGuiWindowFlags_NoSavedSettings)) {
    const auto heading = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(
        "HeaderDrag",
        {std::max(1.f, ImGui::GetContentRegionAvail().x - 155 * scale),
         ImGui::GetFrameHeight()});
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
      const auto p = ImGui::GetWindowPos(), d = ImGui::GetIO().MouseDelta;
      ImGui::SetWindowPos({p.x + d.x, p.y + d.y});
    }
    ImGui::GetWindowDrawList()->AddText({heading.x, heading.y + 5 * scale},
                                        IM_COL32(240, 96, 96, 255),
                                        "CAMERA SETTINGS");
    ImGui::SameLine();
    if (ImGui::Button("Appearance"))
      ImGui::OpenPopup("CameraAppearance");
    ImGui::SameLine();
    if (ImGui::Button("X##Close"))
      show = false;
    if (ImGui::BeginPopup("CameraAppearance")) {
      bool changed = ImGui::SliderFloat("Interface size", &appearance.scale,
                                        .65f, 2, "%.2fx");
      changed |= ImGui::SliderFloat("Font size", &appearance.font_size, 12, 32,
                                    "%.0f px");
      changed |=
          ImGui::SliderFloat("Opacity", &appearance.opacity, .15f, 1, "%.2f");
      changed |= ImGui::ColorEdit3("Window color", appearance.background.data(),
                                   ImGuiColorEditFlags_NoInputs);
      changed |= ImGui::ColorEdit3("Accent color", appearance.accent.data(),
                                   ImGuiColorEditFlags_NoInputs);
      if (ImGui::Button("Restore interface defaults")) {
        appearance = {};
        changed = true;
        ResetOverlayLayout();
      }
      if (ImGui::Button("Reset window position"))
        ResetOverlayLayout();
      if (changed)
        store.SetAppearance(appearance);
      if (ImGui::TreeNode("Connection details")) {
        ImGui::TextWrapped("%s", RuntimeStatus().c_str());
        ImGui::TextWrapped("%s", RuntimeDiagnostics().c_str());
        ImGui::TreePop();
      }
      ImGui::TextUnformatted("F4: open / close. Ctrl+F4: reset position.");
      ImGui::EndPopup();
    }
    const auto observed = CurrentView();
    if (observed != last_observed) {
      if (observed != View::Count) selected = static_cast<int>(observed);
      last_observed = observed;
    }
    static std::string message;
    const auto car = CurrentCar();
    bool custom_scope = false;
    ImGui::BeginChild("CameraControls", {0, -55 * scale});
    if (ImGui::BeginTabBar("camera_scopes")) {
      if (ImGui::BeginTabItem("Global Settings")) {
        const auto view = static_cast<View>(selected);
        auto settings = store.ReadGlobal(view);
        if (EditSettings(settings, view, selected, scale))
          store.SetGlobal(view, settings);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Custom Car Presets")) {
        custom_scope = true;
        ImGui::TextWrapped("Vehicle: %s",
                           car ? CarName(car).c_str()
                               : "Waiting for a vehicle in a race");
        ImGui::BeginDisabled(!car);
        const auto view = static_cast<View>(selected);
        const auto preset = store.ReadCar(car);
        bool enabled = preset.enabled[selected];
        if (ImGui::Checkbox("Use custom preset for this camera", &enabled)) {
          if (enabled)
            store.SetCar(car, view, store.ReadGlobal(view));
          else
            store.UseGlobal(car, view);
        }
        if (!enabled) {
          ImGui::SetNextItemWidth(300 * scale);
          if (ImGui::Combo("Camera", &selected, Names.data(),
                           static_cast<int>(Names.size())))
            RequestView(static_cast<View>(selected));
          ImGui::TextWrapped("Enable a custom preset to edit; otherwise this "
                             "camera follows Global Settings.");
        } else {
          auto settings = store.Effective(car, view);
          if (EditSettings(settings, view, selected, scale))
            store.SetCar(car, view, settings);
        }
        ImGui::EndDisabled();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::BeginDisabled(custom_scope && !car);
    ImGui::PushStyleColor(ImGuiCol_Button, accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, RGB(206, 40, 52));
    if (ImGui::Button("Save", {110 * scale, 0}))
      message = store.Save(message) ? "Settings saved." : message;
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button("Restore Defaults")) {
      store.RestoreDefaults(custom_scope ? car : 0,
                            static_cast<View>(selected));
      message = "Original camera restored.";
    }
    if (custom_scope) {
      ImGui::SameLine();
      if (ImGui::Button("Use Global Settings"))
        store.UseGlobal(car, static_cast<View>(selected));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s",
                        message.empty()
                            ? (store.Dirty() ? "Unsaved changes" : "F4  Close")
                            : message.c_str());
    const auto pos = ImGui::GetWindowPos(), actual = ImGui::GetWindowSize();
    auto layout = store.ReadAppearance();
    if (std::abs(layout.x * area.x - (pos.x - origin.x)) > .6f ||
        std::abs(layout.y * area.y - (pos.y - origin.y)) > .6f ||
        std::abs(layout.width * area.x - actual.x) > .6f ||
        std::abs(layout.height * area.y - actual.y) > .6f) {
      layout.x = (pos.x - origin.x) / area.x;
      layout.y = (pos.y - origin.y) / area.y;
      layout.width = actual.x / area.x;
      layout.height = actual.y / area.y;
      store.SetAppearance(layout);
    }
  }
  ImGui::End();
  ImGui::PopStyleColor(static_cast<int>(std::size(colors)));
  ImGui::PopStyleVar(9);
  ImGui::PopFont();
  if (!show)
    CloseOverlay();
}
} // namespace Ryty::Camera
