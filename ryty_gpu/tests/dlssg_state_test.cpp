#include "ryty_gpu/ryty_dlssg_state.h"

#include <array>
#include <iostream>
#include <stdexcept>

using namespace Vulkan::RytyDlssG;
using namespace std::chrono_literals;

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int main() {
    const Clock::time_point start{};
    std::array<std::uint8_t, 0x128> packet{};
    const auto put = [&](unsigned offset, auto value) {
        std::memcpy(packet.data() + offset, &value, sizeof(value));
    };
    GameplayState gameplay;
    Check(!gameplay.Ready(start), "missing telemetry must suspend generation");
    gameplay.Publish(packet.data(), start);
    Check(!gameplay.Ready(start), "invalid telemetry must suspend generation");
    put(0, std::uint32_t{0x47375330});
    put(0x70, std::uint32_t{1});
    put(0x8e, std::uint16_t{1});
    gameplay.Publish(packet.data(), start);
    Check(gameplay.Ready(start), "on-track telemetry enables stationary cars too");
    for (auto flags : {0u, 3u, 5u, 7u}) {
        put(0x8e, static_cast<std::uint16_t>(flags));
        gameplay.Publish(packet.data(), start);
        Check(!gameplay.Ready(start), "menu, pause and loading must suspend generation");
    }
    put(0x8e, std::uint16_t{1});
    gameplay.Publish(packet.data(), start + 1s);
    Check(!gameplay.Ready(start + 1s), "repeated packets must not revive stale gameplay");
    put(0x70, std::uint32_t{2});
    gameplay.Publish(packet.data(), start + 1s);
    Check(gameplay.Ready(start + 1s), "new packet restores freshness");
    Check(!gameplay.Ready(start + 1251ms), "stopped simulation must suspend generation");

    GenerationGate gate;
    Check(gate.Update(start, false, true, false, true) == Suspension::Gameplay,
          "first loading frame must not generate");
    for (int ms = 16; ms < 1000; ms += 16) {
        Check(gate.Update(start + std::chrono::milliseconds{ms}, true, true, false, false) ==
                  Suspension::Settling,
              "resume requires one second of stable gameplay");
    }
    Check(gate.Update(start + 1000ms, true, true, false, false) == Suspension::None,
          "stable gameplay must resume generation");
    Check(gate.Update(start + 1016ms, true, false, false, false) == Suspension::RepeatedFrame,
          "re-presenting an old picture must not generate frames");
    Check(gate.Update(start + 1032ms, true, true, true, false) == Suspension::Compilation,
          "active shader compilation must suspend generation");
    for (int ms = 1048; ms < 2032; ms += 16) {
        Check(gate.Update(start + std::chrono::milliseconds{ms}, true, true, false, false) ==
                  Suspension::Settling,
              "shader compilation must get a cooldown");
    }
    Check(gate.Update(start + 2032ms, true, true, false, false) == Suspension::None,
          "generation resumes after compilation settles");
    Check(gate.Update(start + 2048ms, true, true, false, true) == Suspension::Resize,
          "resolution change must suspend generation");
    Check(gate.Update(start + 2500ms, true, true, false, false) == Suspension::RepeatedFrame,
          "long presentation gap must suspend generation");
    Check(gate.Update(start + 2516ms, false, true, false, false) == Suspension::Gameplay,
          "return to loading must disable immediately");
    std::cout << "PASS: telemetry flags, stale data, loading, pause, repeated frames, "
                 "compilation cooldown, resize and presentation gaps\n";
}
