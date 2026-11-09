#include "ryty_gpu/ryty_camera_settings.h"
#include <fstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace Ryty::Camera {
namespace {
float Bound(float x, float lo, float hi, float fallback = 0) {
    return std::isfinite(x) ? std::clamp(x, lo, hi) : fallback;
}
nlohmann::json EncodeSettings(const Settings& v) {
    return {{"fov", v.fov}, {"height", v.height}, {"distance", v.distance},
            {"orbit", v.orbit}, {"auto_return", v.auto_return}, {"invert_x", v.invert_x},
            {"invert_y", v.invert_y}, {"sensitivity", v.sensitivity},
            {"smoothing_ms", v.smoothing_ms}, {"return_seconds", v.return_seconds}};
}
Settings ReadSettings(const nlohmann::json& j, View view, bool legacy_sensitivity) {
    if (!j.is_object()) throw std::runtime_error("Camera settings must be an object.");
    Settings s;
    const auto number = [&](const char* key, float& v) {
        if (j.contains(key)) { if (!j[key].is_number()) throw std::runtime_error("Invalid numeric setting."); v = j[key].get<float>(); }
    };
    const auto flag = [&](const char* key, bool& v) {
        if (j.contains(key)) { if (!j[key].is_boolean()) throw std::runtime_error("Invalid switch setting."); v = j[key].get<bool>(); }
    };
    number("fov", s.fov); number("height", s.height); number("distance", s.distance);
    number("sensitivity", s.sensitivity); number("smoothing_ms", s.smoothing_ms);
    number("return_seconds", s.return_seconds);
    flag("orbit", s.orbit); flag("auto_return", s.auto_return);
    flag("invert_x", s.invert_x); flag("invert_y", s.invert_y);
    if (legacy_sensitivity && s.sensitivity == 1.f) s.sensitivity = 0;
    return Sanitize(s, view);
}
}
Settings Sanitize(Settings v) {
    v.fov = v.fov == 0 ? 0 : Bound(v.fov, 30, 100);
    v.height = Bound(v.height, -5, 5); v.distance = Bound(v.distance, -5, 5);
    v.sensitivity = Bound(v.sensitivity, -15, 15, 0);
    v.smoothing_ms = Bound(v.smoothing_ms, 0, 1000, 100);
    v.return_seconds = Bound(v.return_seconds, 0, 10, 2);
    return v;
}
Settings Sanitize(Settings v, View view) {
    v = Sanitize(v);
    const auto limit = Limits(view);
    v.height = Bound(v.height, limit.height_min, limit.height_max);
    v.distance = Bound(v.distance, limit.distance_min, limit.distance_max);
    return v;
}
nlohmann::json Encode(const Document& d) {
    nlohmann::json j{{"version", 1}, {"sensitivity_scale", 2}, {"global", nlohmann::json::object()}, {"cars", nlohmann::json::object()}};
    const auto& a = d.appearance;
    j["appearance"] = {{"theme_version", 2}, {"scale", a.scale}, {"font_size", a.font_size}, {"opacity", a.opacity}, {"background", a.background}, {"accent", a.accent},
        {"x", a.x}, {"y", a.y}, {"width", a.width}, {"height", a.height}};
    for (unsigned i = 0; i < Keys.size(); ++i) j["global"][Keys[i]] = EncodeSettings(d.global[i]);
    for (const auto& [id, car] : d.cars) {
        auto& c = j["cars"][std::to_string(id)]; c = nlohmann::json::object();
        for (unsigned i = 0; i < Keys.size(); ++i) if (car.enabled[i]) c[Keys[i]] = EncodeSettings(car.views[i]);
    }
    return j;
}
bool Decode(const nlohmann::json& j, Document& result, std::string& error) {
    try {
        if (!j.is_object() || j.value("version", 0) != 1) throw std::runtime_error("Unsupported camera preset version.");
        const int sensitivity_scale = j.value("sensitivity_scale", 1);
        if (sensitivity_scale != 1 && sensitivity_scale != 2) throw std::runtime_error("Unsupported sensitivity scale.");
        Document next;
        if (j.contains("appearance")) {
            const auto& a = j["appearance"];
            if (!a.is_object()) throw std::runtime_error("Invalid appearance settings.");
            auto& v = next.appearance;
            v.scale = Bound(a.value("scale", v.scale), 0.65f, 2.0f, 1);
            v.font_size = Bound(a.value("font_size", v.font_size), 12, 32, 17);
            v.opacity = Bound(a.value("opacity", v.opacity), 0.15f, 1, 0.94f);
            v.background = a.value("background", v.background); v.accent = a.value("accent", v.accent);
            v.x = Bound(a.value("x", v.x), 0, 1, 0.10f); v.y = Bound(a.value("y", v.y), 0, 1, 0.04f);
            v.width = Bound(a.value("width", v.width), 0.2f, 1, 0.80f);
            v.height = Bound(a.value("height", v.height), 0.15f, 1, 0.34f);
            for (auto& c : v.background) c = Bound(c, 0, 1);
            for (auto& c : v.accent) c = Bound(c, 0, 1);
            // Migrate the first prototype's exact default palette, preserving deliberate custom colors.
            const auto same_color = [](float x, float y) { return std::abs(x - y) < 0.00001f; };
            if (a.value("theme_version", 0) < 2 && same_color(v.background[0], .055f) &&
                same_color(v.background[1], .06f) && same_color(v.background[2], .075f) &&
                same_color(v.accent[0], .9f) && same_color(v.accent[1], .12f) && same_color(v.accent[2], .19f)) {
                const Document::Appearance defaults;
                v.background = defaults.background; v.accent = defaults.accent;
                if (same_color(v.opacity, .94f)) v.opacity = defaults.opacity;
            }
        }
        if (j.contains("global")) {
            if (!j["global"].is_object()) throw std::runtime_error("Invalid global settings.");
            for (unsigned i = 0; i < Keys.size(); ++i) if (j["global"].contains(Keys[i])) next.global[i] = ReadSettings(j["global"][Keys[i]], static_cast<View>(i), sensitivity_scale == 1);
        }
        if (j.contains("cars")) {
            if (!j["cars"].is_object()) throw std::runtime_error("Invalid car presets.");
            for (const auto& [key, car] : j["cars"].items()) {
                if (key.empty() || key.size() > 10 || key.find_first_not_of("0123456789") != std::string::npos || !car.is_object()) throw std::runtime_error("Invalid car ID.");
                auto id = std::stoull(key);
                if (id == 0 || id > UINT32_MAX || key != std::to_string(id)) throw std::runtime_error("Invalid car ID.");
                CarPreset entry;
                for (unsigned i = 0; i < Keys.size(); ++i) if (car.contains(Keys[i])) { entry.views[i] = ReadSettings(car[Keys[i]], static_cast<View>(i), sensitivity_scale == 1); entry.enabled[i] = true; }
                next.cars.emplace(static_cast<std::uint32_t>(id), entry);
            }
        }
        result = std::move(next); error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
Document Store::Read() const { std::lock_guard lock(mutex); return document; }
Document::Appearance Store::ReadAppearance() const { std::lock_guard lock(mutex); return document.appearance; }
Settings Store::ReadGlobal(View view) const {
    std::lock_guard lock(mutex); const auto i = static_cast<unsigned>(view);
    return i < Names.size() ? document.global[i] : Settings{};
}
CarPreset Store::ReadCar(std::uint32_t car) const {
    std::lock_guard lock(mutex); const auto it = document.cars.find(car);
    return it == document.cars.end() ? CarPreset{} : it->second;
}
Settings Store::Effective(std::uint32_t car, View view) const {
    std::lock_guard lock(mutex); auto i = static_cast<unsigned>(view);
    if (i >= Names.size()) return {};
    auto it = document.cars.find(car);
    return it != document.cars.end() && it->second.enabled[i] ? it->second.views[i] : document.global[i];
}
void Store::SetGlobal(View view, Settings v) {
    auto i = static_cast<unsigned>(view); if (i >= Names.size()) return;
    std::lock_guard lock(mutex); document.global[i] = Sanitize(v, view); ++revision;
}
void Store::SetAppearance(Document::Appearance a) {
    a.scale = Bound(a.scale, 0.65f, 2, 1); a.font_size = Bound(a.font_size, 12, 32, 17);
    a.opacity = Bound(a.opacity, 0.15f, 1, 0.94f);
    for (auto& c : a.background) c = Bound(c, 0, 1);
    for (auto& c : a.accent) c = Bound(c, 0, 1);
    a.x = Bound(a.x, 0, 1, 0.10f); a.y = Bound(a.y, 0, 1, 0.04f);
    a.width = Bound(a.width, 0.2f, 1, 0.80f); a.height = Bound(a.height, 0.15f, 1, 0.34f);
    std::lock_guard lock(mutex); document.appearance = a; ++revision;
}
void Store::SetCar(std::uint32_t car, View view, Settings v) {
    auto i = static_cast<unsigned>(view); if (!car || i >= Names.size()) return;
    std::lock_guard lock(mutex); auto& c = document.cars[car]; c.enabled[i] = true; c.views[i] = Sanitize(v, view); ++revision;
}
void Store::UseGlobal(std::uint32_t car, View view) {
    auto i = static_cast<unsigned>(view); if (i >= Names.size()) return;
    std::lock_guard lock(mutex); auto it = document.cars.find(car);
    if (it == document.cars.end()) return;
    it->second.enabled[i] = false;
    if (std::none_of(it->second.enabled.begin(), it->second.enabled.end(), [](bool v) { return v; })) document.cars.erase(it);
    ++revision;
}
void Store::RestoreDefaults(std::uint32_t car, View view) { if (car) SetCar(car, view, {}); else SetGlobal(view, {}); }
bool Store::Dirty() const { std::lock_guard lock(mutex); return revision != saved_revision; }
bool Store::Load(std::string& error) {
    try {
        if (!std::filesystem::exists(path)) { error.clear(); return true; }
        std::ifstream in(path); if (!in) throw std::runtime_error("Cannot read camera presets.");
        Document next; auto json = nlohmann::json::parse(in);
        if (!Decode(json, next, error)) return false;
        std::lock_guard lock(mutex); document = std::move(next); ++revision; saved_revision = revision;
        error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool Store::Save(std::string& error) {
    try {
        Document snapshot; std::uint64_t version;
        { std::lock_guard lock(mutex); snapshot = document; version = revision; }
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        auto temporary = path; temporary += ".tmp";
        { std::ofstream out(temporary, std::ios::binary | std::ios::trunc); out.exceptions(std::ios::badbit | std::ios::failbit); out << Encode(snapshot).dump(2) << '\n'; out.flush(); out.close(); }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace camera preset file.");
#else
        std::filesystem::rename(temporary, path);
#endif
        { std::lock_guard lock(mutex); saved_revision = version; }
        error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
} // namespace Ryty::Camera
