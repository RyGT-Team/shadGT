// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>

namespace Vulkan::RytyDlssG {

using Clock = std::chrono::steady_clock;

class GameplayState {
public:
    void Publish(const std::uint8_t* data, Clock::time_point now) {
        std::uint32_t magic{}, sequence{};
        std::uint16_t next_flags{};
        std::memcpy(&magic, data, sizeof(magic));
        if (magic != 0x47375330) {
            valid = false;
            return;
        }
        std::memcpy(&sequence, data + 0x70, sizeof(sequence));
        std::memcpy(&next_flags, data + 0x8e, sizeof(next_flags));
        if (!valid || sequence != packet) {
            updated = now;
        }
        packet = sequence;
        flags = next_flags;
        valid = true;
    }

    bool Ready(Clock::time_point now) const {
        return valid && (flags & 7) == 1 && now - updated <= std::chrono::milliseconds{250};
    }

private:
    Clock::time_point updated{};
    std::uint32_t packet{};
    std::uint16_t flags{};
    bool valid{};
};

enum class Suspension { None, Gameplay, RepeatedFrame, Compilation, Resize, Settling };

class GenerationGate {
public:
    Suspension Update(Clock::time_point now, bool gameplay, bool fresh_frame, bool compiling,
                      bool resized) {
        const bool gap = have_frame && now - last_frame > std::chrono::milliseconds{250};
        last_frame = now;
        have_frame = true;
        Suspension reason = Suspension::None;
        if (!gameplay) {
            reason = Suspension::Gameplay;
        } else if (!fresh_frame || gap) {
            reason = Suspension::RepeatedFrame;
        } else if (compiling) {
            reason = Suspension::Compilation;
        } else if (resized) {
            reason = Suspension::Resize;
        }
        if (reason != Suspension::None) {
            resume_after = now + std::chrono::seconds{1};
            return reason;
        }
        return now < resume_after ? Suspension::Settling : Suspension::None;
    }

private:
    Clock::time_point resume_after{};
    Clock::time_point last_frame{};
    bool have_frame{};
};

} // namespace Vulkan::RytyDlssG
