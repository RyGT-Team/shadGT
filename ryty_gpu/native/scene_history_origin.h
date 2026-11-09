// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>
namespace Native {
// Mid-function stub: r12 is the physical scene target, rbx the logical output.
// Preserve flags, xmm0/xmm1 and (for X) rax; change only r14d (X) or eax (Y).
inline std::vector<std::uint8_t> SceneHistoryOriginCode(std::uint32_t scene_hint,
                                                       float scale, bool y) {
    std::vector<std::uint8_t> code;
    const auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
        code.insert(code.end(), bytes.begin(), bytes.end());
    };
    const auto word = [&](std::uint32_t value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        code.insert(code.end(), bytes, bytes + 4);
    };
    emit({0x9c}); // pushfq
    if (!y) emit({0x50}); // push rax
    emit({0x48,0x83,0xec,0x20, 0xf3,0x0f,0x7f,0x04,0x24,
          0xf3,0x0f,0x7f,0x4c,0x24,0x10});
    emit({0x41,0x81,0x7c,0x24,0x44}); word(scene_hint);
    emit({0x0f,0x85}); const auto scene_branch = code.size(); word(0);
    emit({0x81,0x7b,0x44}); word(0x04380780); // logical 1920x1080 output only
    emit({0x0f,0x85}); const auto output_branch = code.size(); word(0);
    if (y) emit({0xf3,0x0f,0x2a,0xc0}); // cvtsi2ss xmm0,eax
    else emit({0xf3,0x41,0x0f,0x2a,0xc6}); // cvtsi2ss xmm0,r14d
    emit({0xb8}); word(std::bit_cast<std::uint32_t>(scale));
    emit({0x66,0x0f,0x6e,0xc8, 0xf3,0x0f,0x59,0xc1});
    if (y) emit({0xf3,0x0f,0x2c,0xc0}); // cvttss2si eax,xmm0
    else emit({0xf3,0x44,0x0f,0x2c,0xf0}); // cvttss2si r14d,xmm0
    const auto done = code.size();
    for (const auto branch : {scene_branch, output_branch}) {
        const auto distance = std::uint32_t(done - branch - 4);
        std::memcpy(code.data() + branch, &distance, 4);
    }
    emit({0xf3,0x0f,0x6f,0x04,0x24, 0xf3,0x0f,0x6f,0x4c,0x24,0x10,
          0x48,0x83,0xc4,0x20});
    if (!y) emit({0x58});
    emit({0x9d}); // popfq
    return code;
}
}
