#include "ryty_gpu/ryty_dlssg_options.h"

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Vulkan::RytyDlssG;
using namespace std::chrono_literals;

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int main() {
    const GenerationOptions::Clock::time_point start{};
    GenerationOptions options;
    std::vector<bool> calls;
    sl::Result response = sl::Result::eWarnOutOfVRAM;
    const auto set = [&](bool enabled) {
        calls.push_back(enabled);
        return response;
    };
    auto update = options.Apply(start, true, set);
    Check(update.applied && update.enabled && calls == std::vector<bool>{true},
          "budget warning must not force generation off or repeat SetOptions");
    for (int ms = 16; ms < 4000; ms += 16) {
        update = options.Apply(start + std::chrono::milliseconds{ms}, true, set);
        Check(update.applied && !update.attempted, "accepted mode must remain cached");
    }
    update = options.Apply(start + 4s, false, set);
    Check(update.applied && !update.enabled && calls == std::vector<bool>({true, false}),
          "loading must switch off even when the SDK reports a budget warning");
    response = sl::Result::eOk;
    Check(options.Apply(start + 5s, true, set).applied, "gameplay must resume");
    Check(options.Apply(start + 6s, false, set).applied, "pause must switch off");
    response = sl::Result::eErrorVulkanAPI;
    update = options.Apply(start + 7s, true, set);
    Check(!update.applied && update.attempted, "actual API failures must remain failures");
    const auto before_stop = calls.size();
    response = sl::Result::eOk;
    update = options.Apply(start + 7016ms, true, set);
    Check(!update.applied && update.attempted && !update.enabled &&
              calls.size() == before_stop + 1 && !calls.back(),
          "hard failure must disable on the next frame without duplicate calls per frame");
    update = options.Apply(start + 7032ms, true, set);
    Check(!update.attempted && !update.applied, "failed enable must back off");
    update = options.Apply(start + 7048ms, false, set);
    Check(update.applied, "enable backoff must never prevent loading from switching off");
    update = options.Apply(start + 9s, true, set);
    Check(update.applied && update.enabled, "generation must recover after the cooldown");
    std::cout << "PASS: budget warning acceptance, no repeated options, loading suspension, "
                 "gameplay resume, hard failure, deferred stop and enable retry\n";
}
