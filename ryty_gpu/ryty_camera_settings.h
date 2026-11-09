#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

namespace Ryty::Camera {
enum class View : unsigned { Cockpit, Chase, Bumper, Roof, Count };
inline constexpr std::array<const char*, 4> Names{"Cockpit", "Chase", "Bumper", "Roof"};
// Serialized keys remain stable for existing camera presets.
inline constexpr std::array<const char*, 4> Keys{"Cockpit", "Chase", "Bumper", "Roof"};
struct PositionLimits { float height_min, height_max, distance_min, distance_max; };
constexpr PositionLimits Limits(View view) {
    switch (view) {
    case View::Cockpit: return {-.6f, .6f, -.6f, .6f};
    case View::Chase: return {-1.f, 1.f, -3.f, 3.f};
    case View::Bumper: return {-5.f, .55f, 0.f, 0.f};
    case View::Roof: return {-.35f, .35f, -1.f, 1.f};
    default: return {};
    }
}
struct Settings {
    // Zero means the original, car-specific FOV. Never invent a default angle.
    float fov = 0, height = 0, distance = 0;
    bool orbit = false, auto_return = true, invert_x = false, invert_y = false;
    float sensitivity = 0, smoothing_ms = 100, return_seconds = 2;
    bool operator==(const Settings&) const = default;
};
inline float OrbitDegreesPerSecond(float sensitivity) {
    const float value = std::isfinite(sensitivity) ? std::clamp(sensitivity, -15.f, 15.f) : 0.f;
    return 120.f * (value < 0 ? 1.f + value / 20.f : 1.f + value / 15.f);
}
struct CarPreset {
    std::array<bool, 4> enabled{};
    std::array<Settings, 4> views{};
    bool operator==(const CarPreset&) const = default;
};
struct Document {
    struct Appearance {
        float scale = 1, font_size = 17, opacity = 0.97f;
        std::array<float, 3> background{30/255.0f, 30/255.0f, 30/255.0f};
        std::array<float, 3> accent{178/255.0f, 24/255.0f, 36/255.0f};
        // Relative to the usable viewport; independent of resolution and DPI.
        float x = 0.10f, y = 0.04f, width = 0.80f, height = 0.34f;
        bool operator==(const Appearance&) const = default;
    } appearance;
    std::array<Settings, 4> global{};
    std::map<std::uint32_t, CarPreset> cars;
    bool operator==(const Document&) const = default;
};
Settings Sanitize(Settings value);
Settings Sanitize(Settings value, View view);
nlohmann::json Encode(const Document& document);
bool Decode(const nlohmann::json& json, Document& result, std::string& error);
// A single mutex protects document publication between the UI and guest camera callbacks.
class Store {
public:
    explicit Store(std::filesystem::path file) : path(std::move(file)) {}
    bool Load(std::string& error);
    bool Save(std::string& error);
    Document Read() const;
    Document::Appearance ReadAppearance() const;
    Settings ReadGlobal(View view) const;
    CarPreset ReadCar(std::uint32_t car) const;
    Settings Effective(std::uint32_t car, View view) const;
    void SetGlobal(View view, Settings settings);
    void SetAppearance(Document::Appearance appearance);
    void SetCar(std::uint32_t car, View view, Settings settings);
    void UseGlobal(std::uint32_t car, View view);
    void RestoreDefaults(std::uint32_t car, View view);
    bool Dirty() const;
private:
    std::filesystem::path path;
    mutable std::mutex mutex;
    Document document;
    std::uint64_t revision = 0, saved_revision = 0;
};
} // namespace Ryty::Camera
