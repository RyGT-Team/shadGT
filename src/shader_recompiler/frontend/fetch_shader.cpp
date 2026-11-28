// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <fmt/format.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/recoverable.h"
#ifdef RYTY_GPU
#include "core/memory.h"
#endif
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/frontend/opcodes.h"

namespace Shader::Gcn {

/**
 * s_load_dwordx4 s[8:11], s[2:3], 0x00
 * s_load_dwordx4 s[12:15], s[2:3], 0x04
 * s_load_dwordx4 s[16:19], s[2:3], 0x08
 * s_waitcnt     lgkmcnt(0)
 * buffer_load_format_xyzw v[4:7], v0, s[8:11], 0 idxen
 * buffer_load_format_xyz v[8:10], v0, s[12:15], 0 idxen
 * buffer_load_format_xy v[12:13], v0, s[16:19], 0 idxen
 * s_waitcnt     0
 * s_setpc_b64   s[0:1]

 * s_load_dwordx4  s[4:7], s[2:3], 0x0
 * s_waitcnt       lgkmcnt(0)
 * buffer_load_format_xyzw v[4:7], v0, s[4:7], 0 idxen
 * s_load_dwordx4  s[4:7], s[2:3], 0x8
 * s_waitcnt       lgkmcnt(0)
 * buffer_load_format_xyzw v[8:11], v0, s[4:7], 0 idxen
 * s_waitcnt       vmcnt(0) & expcnt(0) & lgkmcnt(0)
 * s_setpc_b64     s[0:1]

 * A normal fetch shader looks like the above, the instructions are generated
 * using input semantics on cpu side. Load instructions can either be separate or interleaved
 * We take the reverse way, extract the original input semantics from these instructions.
 **/

AmdGpu::Buffer VertexAttribute::GetSharp(const Shader::Info& info) const noexcept {
    if (t_replay && t_replay->fetch && t_replay->attrib_sharps) { // PB-1: the V# stored for it
        const auto& stored = *static_cast<const FetchShaderData*>(t_replay->fetch);
        for (size_t i = 0; i < stored.attributes.size() && i < t_replay->attrib_sharps->size(); ++i) {
            if (stored.attributes[i] == *this) {
                AmdGpu::Buffer buffer;
                std::memcpy(&buffer, (*t_replay->attrib_sharps)[i].data(), sizeof(buffer));
                buffer.base_address += inst_offset;
                if (data_format) {
                    buffer.data_format = data_format;
                    buffer.num_format = num_format;
                }
                return buffer;
            }
        }
    }
    return GetSharpFromMemory(info);
}

const u32* GetFetchShaderCode(const Info& info, u32 sgpr_base) {
    if (sgpr_base >= info.user_data.size() || info.user_data.size() - sgpr_base < 2) {
        throw Common::RecoverableFailure("fetch shader pointer lies outside the user-data registers");
    }
    if (t_fetch_code_snapshots) {
        for (const auto& snapshot : *t_fetch_code_snapshots) {
            if (snapshot.sgpr_base == sgpr_base) return snapshot.words.data();
        }
        throw Common::RecoverableFailure("queued fetch shader has no owned input snapshot");
    }
    const u32* code;
    std::memcpy(&code, &info.user_data[sgpr_base], sizeof(code));
    if (reinterpret_cast<uintptr_t>(code) < 0x10000 ||
        (reinterpret_cast<uintptr_t>(code) & (alignof(u32) - 1)) != 0) {
        throw Common::RecoverableFailure("fetch shader pointer is null, low, or unaligned");
    }
    return code;
}

thread_local const FetchOverride* t_fetch_override = nullptr; // PERF-GT7-111 (TEST)

struct LastFetch {
    static constexpr size_t MaxWords = 64;
    const u32* address{};
    size_t user_words{};
    size_t count{};
    std::array<u32, MaxWords> words;
    FetchShaderData layout;
};

template <bool Remember>
static bool DecodeFetchShader(const Shader::Info& info, FetchShaderData& out_fetch_data,
                              const u32* code, LastFetch* memo) {
    std::array<u32, LastFetch::MaxWords> observed;
    // Commit a parse only after its return instruction. Failed parses must not leave a
    // partially filled attribute layout that another draw could reuse.
    FetchShaderData parsed;

    struct VsharpLoad {
        u32 dword_offset{};
        u32 base_sgpr{};
        bool valid{};
    };
    std::array<VsharpLoad, 104> loads{};

    const u32* ptr = code;
    constexpr size_t MaxFetchDwords = 1024;
    // FIX-211: what the user data points at is not always a fetch shader when the command
    // thread reads it ahead of the GPU: GT7 crashed here (an instruction no fetch shader has)
    // right after the shader resource walker faulted on unmapped tables. Code that isn't one
    // counts as no fetch shader, with a warning, instead of stopping the game. Input that
    // cannot be read safely (unreadable memory, a truncated owned snapshot) still fails
    // recoverably through Common::RecoverableFailure.
    const auto reject = [&](const char* reason, u32 word) {
        static std::atomic<u32> logged{};
        if (const u32 n = logged++; n < 20 || n % 1000 == 0) {
            LOG_WARNING(Render_Recompiler,
                        "FIX-211: vs {:#x}: no fetch shader at {}: {} (word {:08x} at dword {}), "
                        "drawn without one ({} so far)",
                        info.pgm_hash, fmt::ptr(code), reason, word, ptr - code - 1, n + 1);
        }
        out_fetch_data = {};
        return false;
    };
    const auto read_word = [&]() -> u32 {
        if (t_fetch_code_snapshots) {
            // The snapshot may be shorter than the general parser bound.
            bool available = false;
            for (const auto& snapshot : *t_fetch_code_snapshots) {
                if (snapshot.sgpr_base == info.fetch_shader_sgpr_base) {
                    available = size_t(ptr - code) < snapshot.words.size();
                    break;
                }
            }
            if (!available) {
                throw Common::RecoverableFailure(fmt::format(
                    "invalid fetch shader at {:#x}+{:#x}: truncated owned fetch shader",
                    reinterpret_cast<uintptr_t>(code), (ptr - code) * sizeof(u32)));
            }
        }
        u32 word{};
#ifdef RYTY_GPU
        if (!Core::RytyMemory::GuardedCopy(&word, ptr, sizeof(word))) {
            throw Common::RecoverableFailure(fmt::format(
                "fetch shader input is unreadable at {:#x}", reinterpret_cast<uintptr_t>(ptr)));
        }
#else
        word = *ptr;
#endif
        if constexpr (Remember) {
            const size_t index = ptr - code;
            if (index < observed.size()) observed[index] = word;
        }
        ++ptr;
        return word;
    };
    while (true) {
        if (size_t(ptr - code) >= MaxFetchDwords) {
            return reject("missing return instruction", 0);
        }
        const u32 word0 = read_word();
        const auto encoding = GetInstructionEncoding(word0);

        if (encoding == InstEncoding::SOP1) {
            const auto opcode = OpcodeSOP1((word0 >> 8) & 0xff);
            if (opcode != OpcodeSOP1::S_SETPC_B64) {
                return reject("unsupported scalar instruction", word0);
            }
            break;
        } else if (encoding == InstEncoding::SMRD) {
            const auto opcode = OpcodeSMRD((word0 >> 22) & 0x1f);
            const u32 sdst = (word0 >> 15) & 0x7f;
            const u32 sbase = (word0 >> 9) & 0x3f;
            const u32 offset = word0 & 0xff;
            if (opcode != OpcodeSMRD::S_LOAD_DWORDX4) {
                return reject("unsupported descriptor load", word0);
            }
            if (sdst >= loads.size()) {
                return reject("descriptor destination is out of range", word0);
            }
            if (sbase * 2 + 1 >= info.user_data.size()) {
                return reject("descriptor pointer lies outside user data", word0);
            }
            loads[sdst] = VsharpLoad{offset, sbase * 2, true};
        } else if (encoding == InstEncoding::VOP2) {
            const auto opcode = OpcodeVOP2((word0 >> 25) & 0x3f);
            const u32 src0 = word0 & 0x1ff;
            const u32 vsrc1 = (word0 >> 9) & 0xff;
            const u32 vdst = (word0 >> 17) & 0xff;
            if (opcode != OpcodeVOP2::V_ADD_I32 || src0 >= 16 || vsrc1 != vdst) {
                return reject("unsupported vertex-offset instruction", word0);
            }
            if (vdst == 0) {
                // V0 is always the vertex offset
                parsed.vertex_offset_sgpr = src0;
            } else if (vdst == 3) {
                // V3 is always the instance offset
                parsed.instance_offset_sgpr = src0;
            } else {
                return reject("unsupported vertex-offset destination", word0);
            }
        } else if (encoding == InstEncoding::MUBUF || encoding == InstEncoding::MTBUF) {
            // SRSRC is in units of 4 SPGRs while SBASE is in pairs of SGPRs
            if (size_t(ptr - code) >= MaxFetchDwords) {
                return reject("truncated buffer instruction", word0);
            }
            const u32 word1 = read_word();
            const u32 srsrc = ((word1 >> 16) & 0x1f) * 4;
            if (srsrc >= loads.size() || !loads[srsrc].valid) {
                return reject("buffer instruction has no valid descriptor load", word0);
            }
            u32 num_elements = 0;
            if (encoding == InstEncoding::MUBUF) {
                const auto opcode = OpcodeMUBUF((word0 >> 18) & 0x7f);
                if (opcode < OpcodeMUBUF::BUFFER_LOAD_FORMAT_X ||
                    opcode > OpcodeMUBUF::BUFFER_LOAD_FORMAT_XYZW) {
                    return reject("unsupported buffer instruction", word0);
                }
                num_elements = (std::to_underlying(opcode) -
                                std::to_underlying(OpcodeMUBUF::BUFFER_LOAD_FORMAT_X)) +
                               1;
            } else {
                const auto opcode = OpcodeMTBUF((word0 >> 16) & 0x7);
                if (opcode < OpcodeMTBUF::TBUFFER_LOAD_FORMAT_X ||
                    opcode > OpcodeMTBUF::TBUFFER_LOAD_FORMAT_XYZW) {
                    return reject("unsupported typed-buffer instruction", word0);
                }
                num_elements = (std::to_underlying(opcode) -
                                std::to_underlying(OpcodeMTBUF::TBUFFER_LOAD_FORMAT_X)) +
                               1;
            }
            auto& attrib = parsed.attributes.emplace_back();
            attrib.dest_vgpr = (word1 >> 8) & 0xff;
            attrib.num_elements = num_elements;
            attrib.sgpr_base = loads[srsrc].base_sgpr;
            attrib.dword_offset = loads[srsrc].dword_offset;
            attrib.inst_offset = word0 & 0xfff;
            attrib.instance_data = word1 & 0xff;
            if (encoding == InstEncoding::MTBUF) {
                attrib.data_format = (word0 >> 19) & 0xf;
                attrib.num_format = (word0 >> 23) & 0x7;
            } else {
                attrib.data_format = 0;
                attrib.num_format = 0;
            }
        } else if (encoding == InstEncoding::SOPP) {
            const auto opcode = OpcodeSOPP((word0 >> 16) & 0x7f);
            if (opcode != OpcodeSOPP::S_WAITCNT) {
                return reject("unsupported wait instruction", word0);
            }
        } else if (encoding == InstEncoding::VOP1) {
            const auto opcode = OpcodeVOP1((word0 >> 9) & 0xff);
            const u32 src0 = word0 & 0x1ff;
            const u32 vdst = (word0 >> 17) & 0xff;
            if (opcode != OpcodeVOP1::V_MOV_B32 || src0 != 242) {
                return reject("unsupported vector move", word0);
            }
            if constexpr (Remember) memo = nullptr;
            LOG_WARNING(Render_Recompiler, "Fetch shader has V{} = 1.0 which is ignored", vdst);
        } else {
            return reject("unexpected instruction encoding", word0);
        }
    }

    parsed.size = (ptr - code) * sizeof(u32);
    if constexpr (Remember) {
        const size_t count = ptr - code;
        if (memo && count <= observed.size()) {
            memo->count = 0;
            memo->layout = parsed;
            std::copy_n(observed.begin(), count, memo->words.begin());
            memo->address = code;
            memo->user_words = info.user_data.size();
            memo->count = count;
        }
    }
    out_fetch_data = std::move(parsed);
    return true;
}

bool ParseFetchShader(const Shader::Info& info, FetchShaderData& out_fetch_data) {
    if (!info.has_fetch_shader) {
        return false;
    }

    if (t_replay && t_replay->fetch) { // PB-1: the fetch shader parsed in the game
        out_fetch_data = *static_cast<const FetchShaderData*>(t_replay->fetch);
        return true;
    }
    if (const FetchOverride* o = t_fetch_override) [[unlikely]] {
        out_fetch_data = o->data;
        return o->ok;
    }
    const auto* code = GetFetchShaderCode(info, info.fetch_shader_sgpr_base);
#ifdef RYTY_GPU
    static const bool reuse = [] {
        const char* value = std::getenv("SHADGT_REUSE_FETCH_LAYOUT"); // PERF-SGT-220
        const bool enabled = value && std::strcmp(value, "1") == 0;
        return enabled;
    }();
    if (reuse && !t_fetch_code_snapshots && !t_replay && !Common::t_guest_read_log) {
        static thread_local LastFetch last{};
        if (last.count && last.address == code && last.user_words == info.user_data.size() &&
            Core::RytyMemory::GuardedMatchWords(code, last.words.data(), last.count)) {
            out_fetch_data = last.layout;
            return true;
        }
        return DecodeFetchShader<true>(info, out_fetch_data, code, &last);
    }
#endif
    return DecodeFetchShader<false>(info, out_fetch_data, code, nullptr);
}

} // namespace Shader::Gcn
