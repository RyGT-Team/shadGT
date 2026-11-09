// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// GTS-001: run-time patches of GT Sport's code (CUSA03220 v1.69), applied in DllMain before the
// game's code runs. Each patch checks the original bytes first and is skipped when they differ
// (another game version, another converter build). The game's files are not changed.
//
// RYGT_PATCH=<rva>:<original hex>:<new hex>[,...] applies arbitrary patches the same way, for
// experiments (RVA in GTSport.exe: eboot.bin address + 0x10000).

#include <cstdio>
#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>
#include <windows.h>
#include <fmt/format.h>
#include <intrin.h>

#include "common/diagnostics.h"
#include "common/perf_monitor.h"
#include "common/types.h"
#include "core/debug_state.h"
#include "core/memory.h"
#include "ryty_gpu/native/entry_hook.h"
#include "ryty_gpu/native/menu_snapshot_session.h"
#include "ryty_gpu/native/menu_capture_compile_guard.h"
#include "ryty_gpu/native/scene_history_origin.h"
#include "input/trigger_feedback.h"
#include "ryty_gpu/ryty_dlssg.h"

namespace Vulkan {
void RytyWatchWrites(std::uint64_t address, std::uint32_t ms, std::string label); // DIAG-067
}

namespace {

struct Patch {
    u64 rva;
    std::vector<u8> original;
    std::vector<u8> replacement;
    std::string name;
};

std::vector<u8> ParseHex(const std::string& text) {
    std::vector<u8> bytes;
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        bytes.push_back(static_cast<u8>(std::strtoul(text.substr(i, 2).c_str(), nullptr, 16)));
    }
    return bytes;
}

bool Apply(u8* base, const Patch& patch) {
    u8* target = base + patch.rva;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(target, &info, sizeof(info)) || info.State != MEM_COMMIT) {
        std::fprintf(stderr, "[patch] %s: %#llx is not mapped\n", patch.name.c_str(),
                     static_cast<unsigned long long>(patch.rva));
        return false;
    }
    if (patch.original.size() != patch.replacement.size() ||
        std::memcmp(target, patch.original.data(), patch.original.size()) != 0) {
        std::fprintf(stderr, "[patch] %s: original bytes at %#llx differ, not applied\n",
                     patch.name.c_str(), static_cast<unsigned long long>(patch.rva));
        return false;
    }
    DWORD old_protect = 0;
    if (!VirtualProtect(target, patch.replacement.size(), PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    std::memcpy(target, patch.replacement.data(), patch.replacement.size());
    DWORD ignored = 0;
    VirtualProtect(target, patch.replacement.size(), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patch.replacement.size());
    std::fprintf(stderr, "[patch] %s applied at %#llx\n", patch.name.c_str(),
                 static_cast<unsigned long long>(patch.rva));
    return true;
}

// DIAG-097 (temporary): GT Sport's per-course view distances, GTRender::EnvironmentSetting::Misc
// (reflection field list at eboot 0x2be6ac0; defaults set by its constructor at 0x6c7370). The
// three calls of that constructor are redirected through a stub so every Misc instance is known;
// a thread logs their values when the game changes them (RYGT_ENV_DIAG=1) and can scale chosen
// fields (RYGT_ENV_SCALE=far=0.5,farPassFar=0.5,...) to test how draw counts react.
struct MiscField {
    const char* name;
    u32 offset;
};
constexpr MiscField kMiscFields[] = {
    {"far", 0x0},          {"baseNear", 0x4},        {"extra_far", 0xc},
    {"depthPassFar", 0x10}, {"envFar", 0x18},         {"localEnvFar", 0x1c},
    {"mirrorFar", 0x24},    {"reflectFar", 0x30},     {"farPassBaseNear", 0x34},
    {"farPassFar", 0x38},   {"occluderFar", 0x3c},    {"tree_far", 0xbc},
    {"tree_far_mirror", 0xc0}, {"tree_lod_off0", 0xc4}, {"tree_lod_off1", 0xc8},
    {"tree_lod_off2", 0xcc},
    // EnvironmentSetting::lod (LODSetting at 0x54c): discreteBase, smoothMeshLODBase,
    // nonOnboardLODScale. Only meaningful for instances built by the EnvironmentSetting
    // constructor (site 0).
    {"lod.discreteBase", 0x54c}, {"lod.smoothMeshLODBase", 0x550},
    {"lod.nonOnboardLODScale", 0x558},
};
constexpr u64 kMiscCtor = 0x6c7370;
constexpr u64 kMiscCtorCalls[] = {0x10de5b9, 0x6c712a, 0x6c8faa};
constexpr u64 kTextRvaShift = 0x10000;

using MiscCtorFn = void(PS4_SYSV_ABI*)(void*);
MiscCtorFn g_misc_ctor = nullptr;

struct MiscInstance {
    u8* base;
    int site;
    float seen[std::size(kMiscFields)];    // last value the game left there
    float written[std::size(kMiscFields)]; // last value written by the scale override
    std::string reject;
};
std::mutex g_misc_mutex;
std::vector<MiscInstance> g_misc;
float g_misc_scale[std::size(kMiscFields)];
bool g_misc_any_scale = false;

bool ReadableRange(const void* address, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    return static_cast<const u8*>(address) + size <=
           static_cast<const u8*>(info.BaseAddress) + info.RegionSize;
}

// Instances are often temporaries that the game frees again; one whose memory was reused must
// not be written. Only whole EnvironmentSettings whose near/far planes and four LODSettings all
// hold plausible values pass.
bool LooksLikeEnvironmentSetting(const u8* base, std::string* reject_log = nullptr) {
    if (!ReadableRange(base, 0x590)) {
        return false;
    }
    const auto f = [base](u32 offset) {
        float value;
        std::memcpy(&value, base + offset, 4);
        return value;
    };
    const auto in = [](float value, float lo, float hi) { return value >= lo && value <= hi; };
    if (!in(f(0x0), 10.0f, 1e6f) || !in(f(0x4), 0.001f, 10.0f) || !in(f(0x8), 0.01f, 10.0f) ||
        !in(f(0x14), 0.001f, 100.0f) || !in(f(0x18), 1.0f, 1e6f) || !in(f(0x1c), 1.0f, 1e7f) ||
        !in(f(0x24), 1.0f, 1e6f) || !in(f(0x28), 1.0f, 1e6f) || !in(f(0x2c), 0.001f, 100.0f) ||
        base[0x40] > 1) {
        return false;
    }
    for (const u32 lod : {0x54c, 0x55c, 0x56c, 0x57c}) {
        if (!in(f(lod), 0.0f, 100.0f) || !in(f(lod + 4), 0.0f, 100.0f) || base[lod + 8] > 1 ||
            !in(f(lod + 0xc), 0.001f, 10.0f)) {
            if (reject_log) {
                *reject_log = fmt::format("LODSetting at {:#x}: {} {} {} {}", lod, f(lod),
                                          f(lod + 4), base[lod + 8], f(lod + 0xc));
            }
            return false;
        }
    }
    return true;
}

void MiscMonitor() {
#ifndef NDEBUG
    // RYGT_ENV_AB=<seconds>: the scale is on and off for alternating periods of the same run,
    // with draws per frame and frame rate logged for each state (run-to-run differences of the
    // test race are larger than the effects measured).
    const char* ab_env = Common::DiagEnv("RYGT_ENV_AB");
    const u32 ab_ticks = ab_env ? static_cast<u32>(std::atof(ab_env) * 10.0) : 0;
    struct AbTotals {
        u64 draws, frames, ticks;
    } ab[2]{};
    u64 last_draws = Common::GetWorkCounters().draws.load();
    u64 last_frames = DebugState.GetFrameNum();
    for (u32 tick = 0;; ++tick) {
        Sleep(100);
        const bool scale_on = ab_ticks == 0 || (tick / ab_ticks) % 2 == 0;
        if (ab_ticks) {
            const u64 draws = Common::GetWorkCounters().draws.load();
            const u64 frames = DebugState.GetFrameNum();
            // The first tick after a switch still shows the previous state's frames.
            if (tick % ab_ticks != 0) {
                auto& totals = ab[scale_on ? 0 : 1];
                totals.draws += draws - last_draws;
                totals.frames += frames - last_frames;
                ++totals.ticks;
            }
            last_draws = draws;
            last_frames = frames;
            if (tick % 200 == 199) {
                const auto line = [](const AbTotals& t) {
                    return fmt::format("{:.0f} draws/frame, {:.1f} fps",
                                       double(t.draws) / std::max<u64>(1, t.frames),
                                       t.frames * 10.0 / std::max<u64>(1, t.ticks));
                };
                std::fprintf(stderr, "[env] DIAG-097 A/B: scaled %s; original %s\n",
                             line(ab[0]).c_str(), line(ab[1]).c_str());
                std::fflush(stderr);
                ab[0] = ab[1] = {};
            }
        }
#else
    for (;;) {
        Sleep(100);
        constexpr bool scale_on = true;
#endif
        std::scoped_lock lock{g_misc_mutex};
        for (auto& instance : g_misc) {
            const size_t span = 0x590;
#ifndef NDEBUG
            std::string reject;
            if (instance.site != 0 || !LooksLikeEnvironmentSetting(instance.base, &reject)) {
                if (!reject.empty() && reject != instance.reject) {
                    std::fprintf(stderr, "[env] DIAG-097 misc %p rejected: %s\n", instance.base,
                                 reject.c_str());
                    instance.reject = reject;
                }
                continue;
            }
#else
            if (instance.site != 0 || !LooksLikeEnvironmentSetting(instance.base)) {
                continue;
            }
#endif
            // RYGT_ENV_SET=<hex offset>:<i|f|b><value>,...: fields of every EnvironmentSetting
            // set to a value (graphics-option experiments: shadow splitCount at 0x3d8/0x42c...).
            static const auto env_sets = [] {
                struct Set {
                    u32 offset;
                    char type;
                    double value;
                };
                std::vector<Set> sets;
                if (const char* env = std::getenv("RYGT_ENV_SET")) {
                    std::string text = env;
                    size_t at = 0;
                    while (at < text.size()) {
                        const size_t end = std::min(text.find(',', at), text.size());
                        const std::string item = text.substr(at, end - at);
                        const size_t colon = item.find(':');
                        if (colon != std::string::npos && colon + 2 <= item.size()) {
                            sets.push_back({static_cast<u32>(std::strtoul(item.substr(0, colon).c_str(), nullptr, 16)),
                                            item[colon + 1], std::atof(item.c_str() + colon + 2)});
                        }
                        at = end + 1;
                    }
                }
                return sets;
            }();
            for (const auto& set : env_sets) {
                if (!scale_on || set.offset + 4 > span) {
                    continue;
                }
                u8* field = instance.base + set.offset;
                if (set.type == 'i') {
                    s32 old;
                    std::memcpy(&old, field, 4);
                    const s32 value = static_cast<s32>(set.value);
                    if (old != value) {
                        std::fprintf(stderr, "[env] RYGT_ENV_SET %p+%#x: %d -> %d\n", instance.base, set.offset, old, value);
                        std::memcpy(field, &value, 4);
                    }
                } else if (set.type == 'f') {
                    float old;
                    std::memcpy(&old, field, 4);
                    const float value = static_cast<float>(set.value);
                    if (old != value) {
                        std::fprintf(stderr, "[env] RYGT_ENV_SET %p+%#x: %g -> %g\n", instance.base, set.offset, old, value);
                        std::memcpy(field, &value, 4);
                    }
                } else if (set.type == 'b') {
                    const u8 value = static_cast<u8>(set.value);
                    if (*field != value) {
                        std::fprintf(stderr, "[env] RYGT_ENV_SET %p+%#x: %u -> %u\n", instance.base, set.offset, *field, value);
                        *field = value;
                    }
                }
            }
#ifndef NDEBUG
            std::string changes;
#endif
            for (size_t i = 0; i < std::size(kMiscFields); ++i) {
                if (kMiscFields[i].offset >= span) {
                    continue;
                }
                float* field = reinterpret_cast<float*>(instance.base + kMiscFields[i].offset);
                const float value = *field;
                if (value == instance.written[i]) {
                    if (!scale_on) {
                        *field = instance.seen[i]; // A/B: back to the game's value
                        instance.written[i] = -1.0f;
                    }
                    continue; // our own override
                }
                if (value != instance.seen[i]) {
#ifndef NDEBUG
                    changes += fmt::format(" {}={}", kMiscFields[i].name, value);
#endif
                    instance.seen[i] = value;
                }
                // A copy of another instance's already scaled value is not scaled again.
                const bool copied_override = std::ranges::any_of(
                    g_misc, [&](const MiscInstance& other) { return other.written[i] == value; });
                if (scale_on && g_misc_scale[i] != 1.0f && value > 0.0f && !copied_override) {
                    const float scaled = value * g_misc_scale[i];
                    *field = scaled;
                    instance.written[i] = scaled;
                }
            }
#ifndef NDEBUG
            if (!changes.empty()) {
                std::fprintf(stderr, "[env] DIAG-097 misc %p (site %d):%s\n", instance.base,
                             instance.site, changes.c_str());
                std::fflush(stderr);
            }
            // RYGT_ENV_WATCH=<hex offset>: which code reads that field of the first instance that
            // gets a course's values (a hardware read/write watch, at once and again 40 s later).
            static const char* watch_env = Common::DiagEnv("RYGT_ENV_WATCH");
            static const u32 watch_offset =
                watch_env ? static_cast<u32>(std::strtoul(watch_env, nullptr, 16)) : 0;
            static u8* watched = nullptr;
            static u32 watch_tick = 0;
            float far_plane;
            std::memcpy(&far_plane, instance.base, 4);
            if (watch_env && !watched && far_plane != 1000.0f) {
                watched = instance.base;
                watch_tick = tick;
                Vulkan::RytyWatchWrites(reinterpret_cast<u64>(watched) + watch_offset, 3000,
                                        fmt::format("rw env+{:#x}", watch_offset));
            } else if (watch_env && watched == instance.base && tick == watch_tick + 400) {
                Vulkan::RytyWatchWrites(reinterpret_cast<u64>(watched) + watch_offset, 2000,
                                        fmt::format("rw env+{:#x} later", watch_offset));
            }
#endif
        }
    }
}

void RecordMisc(void* self, int site) {
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread{MiscMonitor}.detach();
    });
    std::scoped_lock lock{g_misc_mutex};
    for (auto& instance : g_misc) {
        if (instance.base == self) {
            instance.site = site;
            std::fill(std::begin(instance.seen), std::end(instance.seen), -1.0f);
            return;
        }
    }
    MiscInstance instance{static_cast<u8*>(self), site, {}, {}, {}};
    std::fill(std::begin(instance.seen), std::end(instance.seen), -1.0f);
    std::fill(std::begin(instance.written), std::end(instance.written), -1.0f);
    g_misc.push_back(instance);
#ifndef NDEBUG
    // RYGT_ENV_WATCH_NEW=<hex offset>: reads and writes of that field of each new
    // EnvironmentSetting for its first 1.5 s (skipped while another watch runs).
    // (Not during boot: the watch logs through the renderer's log, which starts later.)
    static const auto first_record = std::chrono::steady_clock::now();
    if (const char* env = Common::DiagEnv("RYGT_ENV_WATCH_NEW");
        env && site == 0 && std::chrono::steady_clock::now() - first_record > std::chrono::seconds{30}) {
        const u32 offset = static_cast<u32>(std::strtoul(env, nullptr, 16));
        Vulkan::RytyWatchWrites(reinterpret_cast<u64>(self) + offset, 1500,
                                fmt::format("rw new env {} +{:#x}", self, offset));
    }
#endif
}

template <int Site>
void PS4_SYSV_ABI MiscCtorHook(void* self) {
    g_misc_ctor(self);
    RecordMisc(self, Site);
}

// Memory for jump stubs within +-2 GB of the game's code, so its rel32 calls can reach them.
u8* AllocNear(u8* base) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const u64 granularity = system.dwAllocationGranularity;
    for (u64 distance = granularity; distance < (1ULL << 30); distance += granularity) {
        for (const s64 sign : {-1, 1}) {
            const u64 address =
                (reinterpret_cast<u64>(base) + sign * static_cast<s64>(distance)) & ~(granularity - 1);
            if (void* memory = VirtualAlloc(reinterpret_cast<void*>(address), 0x1000,
                                            MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
                return static_cast<u8*>(memory);
            }
        }
    }
    return nullptr;
}

// GTS-002: the Lewis Hamilton Time Trial Challenge. v1.69 is Polyphony's end-of-service build: its
// main menu (gtmode.ad, setupDisplay) shows the mode only when the player's inventory says the free
// add-on was downloaded, i.e. when the KeyValue ints unlock_dlc_gm_LewisHamilton and
// dlc_gm_validLewisHamilton are 1. The servers that set them are gone, but all ten events, Lewis's
// ghosts and voice-overs are in the game's files. The KeyValue lookup that MKeyValue::getInt calls
// answers 1 for those two keys; every other key is untouched. RYGT_LEWIS=0 turns this off.
constexpr u64 kKeyValueGetIntCall = 0x212204; // call of the lookup inside MKeyValue::getInt
constexpr u64 kKeyValueLookup = 0xbae080;     // bool lookup(store, const char* key, s64* value)

using KeyValueLookupFn = bool(PS4_SYSV_ABI*)(void*, const char*, s64*);
KeyValueLookupFn g_key_value_lookup = nullptr;

// GTS-004: every special paint (matte, chrome, colour-shift and maker colours, bought with mileage
// points from the shut-down servers). The inventory (inventory.ad) asks KeyValue for
// unlock_sp_paint_<id>; those answer 1. Off by default (Lance: the paints don't show on the car);
// RYGT_PAINTS=1 turns it on.
bool g_unlock_lewis = false;
bool g_unlock_paints = false;

bool PS4_SYSV_ABI KeyValueLookupHook(void* store, const char* key, s64* value) {
    const bool found = g_key_value_lookup(store, key, value);
    if (key && g_unlock_lewis && (std::strcmp(key, "unlock_dlc_gm_LewisHamilton") == 0 ||
                                  std::strcmp(key, "dlc_gm_validLewisHamilton") == 0)) {
        *value = 1;
        return true;
    }
    if (key && g_unlock_paints && std::strncmp(key, "unlock_sp_paint", 15) == 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "[patch] GTS-004 special paints unlocked (first key %s)\n", key);
        }
        *value = 1;
        return true;
    }
    return found;
}

// Points the rel32 call at `call_rva` (which must call `target_rva`) at `hook` through a stub.
bool RedirectCall(u8* base, u8* stub, u64 call_rva, u64 target_rva, void* hook, const char* name) {
    u8* call = base + call_rva + kTextRvaShift;
    s32 rel = 0;
    std::memcpy(&rel, call + 1, 4);
    if (call[0] != 0xe8 || call + 5 + rel != base + target_rva + kTextRvaShift) {
        std::fprintf(stderr, "[patch] %s: call at %#llx differs, not applied\n", name,
                     static_cast<unsigned long long>(call_rva));
        return false;
    }
    const u8 jump[6] = {0xff, 0x25, 0, 0, 0, 0}; // jmp qword ptr [rip]
    std::memcpy(stub, jump, 6);
    std::memcpy(stub + 6, &hook, 8);
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    const s32 new_rel = static_cast<s32>(stub - (call + 5));
    DWORD old_protect = 0;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    std::memcpy(call + 1, &new_rel, 4);
    DWORD ignored = 0;
    VirtualProtect(call, 5, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    std::fprintf(stderr, "[patch] %s applied at %#llx\n", name,
                 static_cast<unsigned long long>(call_rva));
    return true;
}

void InstallLewisChallenge(u8* base) {
    const char* env = std::getenv("RYGT_LEWIS");
    const char* paints = std::getenv("RYGT_PAINTS");
    g_unlock_lewis = !(env && env[0] == '0');
    g_unlock_paints = paints && paints[0] == '1'; // off: they unlock but don't show on the car
    if (!g_unlock_lewis && !g_unlock_paints) {
        return;
    }
    u8* stub = AllocNear(base);
    if (!stub) {
        std::fprintf(stderr, "[patch] GTS-002: no memory near the game's code for the hook\n");
        return;
    }
    g_key_value_lookup = reinterpret_cast<KeyValueLookupFn>(base + kKeyValueLookup + kTextRvaShift);
    RedirectCall(base, stub, kKeyValueGetIntCall, kKeyValueLookup,
                 reinterpret_cast<void*>(&KeyValueLookupHook), "GTS-002 Lewis Hamilton Challenge");
}

// GTS-003: the Livery Editor offline. The main menu (gtmode.ad, setupDisplay) disables the Livery
// button whenever the game is offline (`disable: [!enable_menu || DisableLivery]`, enable_menu =
// online), although the editor itself has an offline mode that saves liveries locally. The menu
// script is patched while the game reads it: that one condition uses completed_playgo (true once the
// game is installed) instead of enable_menu, like the Garage button. The bytes are only changed
// where the whole original sequence (78 bytes of v1.69's GTMODE.ADC) is found in one 4 KB read.
// RYGT_LIVERY=0 turns this off.
struct ScriptPatch {
    const char* name;
    std::vector<u8> original;
    size_t offset; // of `replacement` inside `original`
    std::vector<u8> replacement;
};
std::vector<ScriptPatch> g_script_patches;

struct GuestIoVec {
    u8* base;
    u64 length;
};
// The game's file read (eboot 0x1be7930); its buffered streams refill 4 KB at a time from these
// three calls, which is how scripts are read.
constexpr u64 kFileRead = 0x1be7930;
constexpr u64 kStreamRefillCalls[] = {0x17626ef, 0x170f88e, 0x1762a38};
using FileReadFn = s64(PS4_SYSV_ABI*)(void*, GuestIoVec*);
FileReadFn g_file_read = nullptr;

// GTS-005: whole-script replacements. A file in <exe dir>\patches\scripts replaces the game script
// whose symbol table starts the same way (its own path names, which a patched script keeps). It is served chunk by chunk as the game reads the original, so it may not be larger
// than the original (adcpatch pads it with zeros to the original size).
struct ScriptReplacement {
    std::string name;
    std::vector<u8> data;
};
std::vector<ScriptReplacement> g_script_replacements;
std::mutex g_replacement_mutex;
std::vector<std::pair<void*, std::pair<const ScriptReplacement*, size_t>>> g_active_replacements;

// The bytes after the "ADCH012" header and the symbol count (which grows when a patch adds names):
// the start of the symbol table, i.e. the script's own path names.
const u8* ScriptKey(const u8* data, size_t size) {
    if (size < 8 + 4 + 120 || std::memcmp(data, "ADCH012", 8) != 0) {
        return nullptr;
    }
    size_t extra = 0;
    for (u8 b = data[8]; b & 0x80; b <<= 1) {
        ++extra;
    }
    return data + 8 + 1 + extra;
}

void ServeReplacement(void* file, u8* begin, s64 read) {
    std::scoped_lock lock{g_replacement_mutex};
    auto it = std::find_if(g_active_replacements.begin(), g_active_replacements.end(),
                           [file](const auto& entry) { return entry.first == file; });
    // FIX-105: the game stops reading a script once it has parsed it, so a replacement smaller than
    // the original (zero padding at the end) can stay active. The next script read through the same
    // stream object then got the padding (zeros) instead of its own bytes: GRIM.ADC with the
    // livery ban checks replaced left NP.ADC empty ("stream is not adhoc script code") and the game
    // crashed at boot. A read that starts with a script header is always a new script.
    if (it != g_active_replacements.end() && read >= 8 && std::memcmp(begin, "ADCH012", 8) == 0) {
        g_active_replacements.erase(it);
        it = g_active_replacements.end();
    }
    if (it == g_active_replacements.end()) {
        const u8* key = ScriptKey(begin, static_cast<size_t>(read));
        for (const auto& replacement : g_script_replacements) {
            const u8* own = ScriptKey(replacement.data.data(), replacement.data.size());
            if (key && own && std::memcmp(key, own, 120) == 0) {
                g_active_replacements.push_back({file, {&replacement, 0}});
                it = std::prev(g_active_replacements.end());
                std::fprintf(stderr, "[patch] GTS-005 serving %s\n", replacement.name.c_str());
                break;
            }
        }
        if (it == g_active_replacements.end()) {
            return;
        }
    }
    auto& [replacement, offset] = it->second;
    const size_t available = offset < replacement->data.size() ? replacement->data.size() - offset : 0;
    const size_t count = std::min<size_t>(available, static_cast<size_t>(read));
    std::memcpy(begin, replacement->data.data() + offset, count);
    std::memset(begin + count, 0, static_cast<size_t>(read) - count);
    offset += static_cast<size_t>(read);
    if (offset >= replacement->data.size()) {
        g_active_replacements.erase(it);
    }
}

void LoadScriptReplacements() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir{exe};
    dir = dir.substr(0, dir.find_last_of("\\/") + 1) + "patches\\scripts\\";
    WIN32_FIND_DATAA found{};
    HANDLE find = FindFirstFileA((dir + "*.ADC").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        FILE* f = std::fopen((dir + found.cFileName).c_str(), "rb");
        if (!f) {
            continue;
        }
        ScriptReplacement replacement{found.cFileName, {}};
        std::fseek(f, 0, SEEK_END);
        replacement.data.resize(static_cast<size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        const size_t got = std::fread(replacement.data.data(), 1, replacement.data.size(), f);
        std::fclose(f);
        if (got == replacement.data.size() && got >= 128) {
            std::fprintf(stderr, "[patch] GTS-005 script replacement %s (%zu bytes)\n",
                         found.cFileName, got);
            g_script_replacements.push_back(std::move(replacement));
        }
    } while (FindNextFileA(find, &found));
    FindClose(find);
}

// FIX-110: whether a race (a race_* project) or the menus (gtmode, arcade and the other menu
// projects) run, from the project script the game loads when it switches. Overlays a race loads
// (dialogs, the meter, the pause menu) don't change it. The renderer skips draws for new
// pipelines only in races; in the menus some scenes draw once into a texture they keep showing.
std::atomic<bool> g_in_race{false};


extern "C" void RytyReplayVideoSetReplay(bool replay); // GTS-015, gts_replay_video.cpp

void NoteProjectScript(const u8* data, s64 read) {
    if (read < 16 || std::memcmp(data, "ADCH012", 8) != 0) {
        return;
    }
    const std::string_view head{reinterpret_cast<const char*>(data),
                                static_cast<size_t>(std::min<s64>(read, 200))};
    const auto at = head.find("projects/gt7sp/");
    if (at == std::string_view::npos) {
        return;
    }
    std::string_view name = head.substr(at + 15);
#ifndef NDEBUG
    if (std::getenv("RYGT_LOG_PROJECTS")) {
        // DIAG-135: every project script the game loads, with the time.
        SYSTEMTIME now;
        GetLocalTime(&now);
        const std::string_view path = name.substr(0, name.find_first_of(".\0", 0));
        std::fprintf(stderr, "[projects] %02d:%02d:%02d.%03d %.*s\n", now.wHour, now.wMinute,
                     now.wSecond, now.wMilliseconds, static_cast<int>(std::min<size_t>(path.size(), 80)),
                     path.data());
    }
#endif
    name = name.substr(0, name.find('/'));
    static constexpr std::string_view menus[] = {
        "gtmode", "arcade", "campaign", "garage", "brand_central", "gallery", "livery2",
        "photo", "play_photo", "option", "ps_store", "car_select", "course_select",
        "event_setting", "preview", "car_preview", "online", "boot", "demo"};
    // "race" itself is the races' shared script, which the game also loads at boot (before the
    // gift car's thumbnail is drawn); a race runs one of the race_* projects.
    const bool race = name.starts_with("race_");
    if (!race && std::find(std::begin(menus), std::end(menus), name) == std::end(menus)) {
        return;
    }
    RytyReplayVideoSetReplay(name == "race_replay"); // GTS-015
    if (g_in_race.exchange(race) != race) {
        std::fprintf(stderr, "[patch] FIX-110 %.*s: %s\n", static_cast<int>(name.size()),
                     name.data(), race ? "race, new pipelines are built in the background"
                                       : "menus, draws wait for new pipelines");
    }
}

#ifndef NDEBUG
// DIAG-163: the first 60 scripts the game loads, with their source path and the time since the
// first one, so a boot that ends early shows how far its scripts got. RYGT_LOG_BOOT=0 turns it off.
void NoteBootScript(const u8* data, s64 read) {
    static const bool off = std::getenv("RYGT_LOG_BOOT") && std::getenv("RYGT_LOG_BOOT")[0] == '0';
    static std::atomic<int> count{0};
    static const auto first = std::chrono::steady_clock::now();
    if (off || read < 16 || std::memcmp(data, "ADCH012", 8) != 0) {
        return;
    }
    const int number = ++count;
    if (number > 60) {
        return;
    }
    // After the header: the symbol count (one byte, plus one more per leading 1 bit of the first,
    // as ScriptKey reads it), then length-prefixed names; the first is the compiled unit
    // (_build/.works/gt7sp/network/network.adl, out/.work/projects/gt7sp/gallery/_tmp_gallery.adi).
    size_t at = 9;
    for (u8 b = data[8]; b & 0x80; b <<= 1) {
        ++at;
    }
    std::string name = "?";
    if (at < size_t(read) && !(data[at] & 0x80) && at + 1 + data[at] <= size_t(read)) {
        name.assign(reinterpret_cast<const char*>(data + at + 1), data[at]);
    }
    for (const std::string_view prefix : {"_build/.works/", "out/.work/"}) {
        if (name.starts_with(prefix)) {
            name.erase(0, prefix.size());
        }
    }
    for (char& c : name) {
        c = (c >= 32 && c < 127) ? c : '.';
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - first)
                        .count();
    std::fprintf(stderr, "[boot] script %d (+%lld ms): %s\n", number, static_cast<long long>(ms),
                 name.c_str());
}

#endif

s64 PS4_SYSV_ABI StreamRefillHook(void* file, GuestIoVec* iov) {
    const s64 read = g_file_read(file, iov);
    if (read <= 0 || !iov || !iov->base) {
        return read;
    }
    u8* const begin = iov->base;
    u8* const end = begin + read;
    if (!g_script_replacements.empty()) {
        ServeReplacement(file, begin, read);
    }
#ifndef NDEBUG
    NoteBootScript(begin, read); // DIAG-163
#endif
    NoteProjectScript(begin, read); // FIX-110
#ifndef NDEBUG
    static const bool log_scripts = std::getenv("RYGT_LOG_SCRIPTS") != nullptr;
    if (log_scripts) {
        // DIAG-107: the start of each script chunk read (first 48 printable bytes).
        std::string text;
        for (s64 i = 0; i < std::min<s64>(read, 160) && text.size() < 120; ++i) {
            text += (begin[i] >= 32 && begin[i] < 127) ? static_cast<char>(begin[i]) : '.';
        }
        std::fprintf(stderr, "[scripts] file %p read %lld: %s\n", file, static_cast<long long>(read), text.c_str());
    }
#endif
    for (const auto& patch : g_script_patches) {
        u8* found = std::search(begin, end, patch.original.begin(), patch.original.end());
        if (found != end) {
            std::memcpy(found + patch.offset, patch.replacement.data(), patch.replacement.size());
            std::fprintf(stderr, "[patch] %s applied to the menu script\n", patch.name);
        }
    }
    return read;
}

#ifndef NDEBUG
// PB-3 (RYGT_SCAN_READS=1): the game's other file reads (all calls of the read function but the
// script streams') are scanned for PS4 shader programs, to see which shaders a loading screen
// brings in before anything is drawn with them.
constexpr u64 kOtherReadCalls[] = {
    0x5cc8ea,  0x5cc91b,  0x5d84c5,  0xf25606,  0xf25701,  0xf257ff,  0xf2ed8a,  0x1392444,
    0x180f065, 0x180f140, 0x18142cc, 0x183950f, 0x1839580, 0x1839c58, 0x183a258, 0x183ca63,
    0x183cc19, 0x183d377, 0x1877647, 0x18776a9, 0x18776db, 0x18777c4, 0x18c3de5, 0x18ddc63,
    0x1a5c9dd, 0x1a5d5ba, 0x1a5d9a0, 0x1a5e39c, 0x1a5e404, 0x1a5e4fd, 0x1a5e887, 0x1a5e8d3,
    0x1a5ecbf, 0x1be2785, 0x1be75eb, 0x1be8458, 0x1beea2a, 0x1beea91, 0x1beebf1, 0x1beecd5,
    0x1bef7d0, 0x1bef8cb, 0x1ca59cb};
std::mutex g_scan_mutex;
std::unordered_set<u64> g_scanned_programs;
u64 g_scan_reads = 0, g_scan_bytes = 0, g_scan_programs = 0;
auto g_scan_window = std::chrono::steady_clock::now();

s64 PS4_SYSV_ABI ScanReadHook(void* file, GuestIoVec* iov) {
    const s64 read = g_file_read(file, iov);
    if (read <= 0 || !iov || !iov->base) {
        return read;
    }
    const std::string_view data{reinterpret_cast<const char*>(iov->base), size_t(read)};
    u64 found = 0, fresh = 0;
    for (size_t at = data.find("OrbShdr"); at != std::string_view::npos && at + 32 <= data.size();
         at = data.find("OrbShdr", at + 7)) {
        u64 hash;
        std::memcpy(&hash, data.data() + at + 16, sizeof(hash));
        ++found;
        std::scoped_lock lock{g_scan_mutex};
        fresh += g_scanned_programs.insert(hash).second ? 1 : 0;
    }
    std::scoped_lock lock{g_scan_mutex};
    ++g_scan_reads;
    g_scan_bytes += u64(read);
    g_scan_programs += fresh;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_scan_window >= std::chrono::seconds{2} && g_scan_reads != 0) {
        std::fprintf(stderr, "[patch] PB-3 reads in 2 s: %llu (%llu MB), %llu new shader programs (%zu in all)\n",
                     static_cast<unsigned long long>(g_scan_reads),
                     static_cast<unsigned long long>(g_scan_bytes >> 20),
                     static_cast<unsigned long long>(g_scan_programs), g_scanned_programs.size());
        g_scan_reads = g_scan_bytes = g_scan_programs = 0;
        g_scan_window = now;
    }
    return read;
}

void InstallShaderLoadScan(u8* base) {
    if (!(std::getenv("RYGT_SCAN_READS") && std::getenv("RYGT_SCAN_READS")[0] == '1')) {
        return;
    }
    u8* stubs = AllocNear(base);
    if (!stubs) {
        return;
    }
    g_file_read = reinterpret_cast<FileReadFn>(base + kFileRead + kTextRvaShift);
    for (size_t i = 0; i < std::size(kOtherReadCalls); ++i) {
        RedirectCall(base, stubs + i * 16, kOtherReadCalls[i], kFileRead,
                     reinterpret_cast<void*>(&ScanReadHook), "PB-3 file reads");
    }
}

#endif

void InstallScriptPatches(u8* base) {
    const char* livery = std::getenv("RYGT_LIVERY");
    if (!(livery && livery[0] == '0')) {
        // STRING_CONST 'disable', ARRAY_CONST 1, VARIABLE_EVAL enable_menu [7], UNARY_OPERATOR
        // __not__, LOGICAL_OR, VARIABLE_EVAL VariableManager.get, STRING_CONST
        // 'GameDemo::DisableLivery', CALL 1. enable_menu [7] becomes completed_playgo [2].
        g_script_patches.push_back(
            {"GTS-003 Livery Editor offline",
             ParseHex("940200001d811e940200003201000000940200002601000000811c070000009402000022160000"
                      "0000407001000094020000260300000076777800000000940200001d8142940200000601000000"),
             25, ParseHex("811302000000")});
    }
    if (const char* menu = std::getenv("RYGT_DEBUG_MENU"); !(menu && menu[0] == '0')) {
        // GTS-013: the Debug Menu button stays in the quick menu with GTS-012 (which makes
        // GetVersionBuild() "no-build"). overlay_menu2's WINDOWS.onInitialize shows
        // alias_MainMenu.alias_Button_DebugMenu where GetVersionBuild() == "debug" (or
        // Grim3.isDebug(), or UserData::DebugMenu); that == becomes != (BINARY_OPERATOR symbol 8
        // __eq__ -> 0x17 __ne__ in this script). VARIABLE_EVAL GetVersionBuild, CALL 0,
        // STRING_CONST 'debug', BINARY_OPERATOR, at line 402. Every other debug check stays retail.
        g_script_patches.push_back(
            {"GTS-013 quick menu Debug Menu button",
             ParseHex("260200000080d980d907000000920100000600000000920100001d80da920100000508"),
             34, ParseHex("17")});
        // GTS-016: the Replays menu entry the game keeps for its developer MoTeC export, which
        // RyGT's patched gallery script (adcpatch gallery.ad) turns into "Export this Video"
        // (GTS-019). The script adds the entry only when GetVersionBuild() == "debug" (line 465), so
        // the retail build (GTS-012) hid it; that __eq__ (symbol 0x3b) becomes __ne__ (0x40).
        g_script_patches.push_back(
            {"GTS-016 Replays menu Export this Video",
             ParseHex("d1010000260200000081558155000000"
                      "00d101000006000000"
                      "00d10100001d8156d101000005"
                      "3b000000"),
             38, ParseHex("40")});
    }
    if (!(std::getenv("RYGT_SCRIPTS") && std::getenv("RYGT_SCRIPTS")[0] == '0')) {
        LoadScriptReplacements();
    }
    // The hook stays in without patches too: FIX-110 reads which project the game loads.
    u8* stubs = AllocNear(base);
    if (!stubs) {
        std::fprintf(stderr, "[patch] GTS-003: no memory near the game's code for the hooks\n");
        return;
    }
    g_file_read = reinterpret_cast<FileReadFn>(base + kFileRead + kTextRvaShift);
    for (size_t i = 0; i < std::size(kStreamRefillCalls); ++i) {
        RedirectCall(base, stubs + i * 16, kStreamRefillCalls[i], kFileRead,
                     reinterpret_cast<void*>(&StreamRefillHook), "GTS-003 script reads");
    }
#ifndef NDEBUG
    InstallShaderLoadScan(base);
#endif
}

void InstallMiscHooks(u8* base) {
#ifndef NDEBUG
    const char* diag = Common::DiagEnv("RYGT_ENV_DIAG");
#else
    constexpr const char* diag = nullptr;
#endif
    const char* scale = std::getenv("RYGT_ENV_SCALE");
    if (!(diag && diag[0] == '1') && !scale && !std::getenv("RYGT_ENV_SET")) {
        return;
    }
    std::fill(std::begin(g_misc_scale), std::end(g_misc_scale), 1.0f);
    if (scale) {
        std::string list{scale};
        for (size_t start = 0; start < list.size();) {
            const size_t end = std::min(list.find(',', start), list.size());
            const std::string item = list.substr(start, end - start);
            const size_t eq = item.find('=');
            for (size_t i = 0; eq != std::string::npos && i < std::size(kMiscFields); ++i) {
                if (item.compare(0, eq, kMiscFields[i].name) == 0 &&
                    std::strlen(kMiscFields[i].name) == eq) {
                    g_misc_scale[i] = std::strtof(item.c_str() + eq + 1, nullptr);
                    g_misc_any_scale = true;
                    std::fprintf(stderr, "[env] scaling %s by %g\n", kMiscFields[i].name,
                                 g_misc_scale[i]);
                }
            }
            start = end + 1;
        }
    }
    u8* stubs = AllocNear(base);
    if (!stubs) {
        std::fprintf(stderr, "[env] no memory near the game's code for hook stubs\n");
        return;
    }
    g_misc_ctor = reinterpret_cast<MiscCtorFn>(base + kMiscCtor + kTextRvaShift);
    void* hooks[] = {reinterpret_cast<void*>(&MiscCtorHook<0>),
                     reinterpret_cast<void*>(&MiscCtorHook<1>),
                     reinterpret_cast<void*>(&MiscCtorHook<2>)};
    for (size_t i = 0; i < std::size(kMiscCtorCalls); ++i) {
        u8* call = base + kMiscCtorCalls[i] + kTextRvaShift;
        s32 rel = 0;
        std::memcpy(&rel, call + 1, 4);
        if (call[0] != 0xe8 || call + 5 + rel != base + kMiscCtor + kTextRvaShift) {
            std::fprintf(stderr, "[env] call at %#llx is not the Misc constructor, not hooked\n",
                         static_cast<unsigned long long>(kMiscCtorCalls[i]));
            continue;
        }
        u8* stub = stubs + i * 16;
        const u8 jump[6] = {0xff, 0x25, 0, 0, 0, 0}; // jmp qword ptr [rip]
        std::memcpy(stub, jump, 6);
        std::memcpy(stub + 6, &hooks[i], 8);
        const s32 new_rel = static_cast<s32>(stub - (call + 5));
        DWORD old_protect = 0;
        VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old_protect);
        std::memcpy(call + 1, &new_rel, 4);
        DWORD ignored = 0;
        VirtualProtect(call, 5, old_protect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, 0x1000);
    std::fprintf(stderr, "[env] DIAG-097 Misc constructor hooks installed\n");
}

} // namespace

#ifndef NDEBUG
// DIAG-106: the game's text-entry routine (eboot 0xf34700, this, pad slot) opens the keyboard
// dialog or the inline keyboard. It refuses when the slot is above 3, a keyboard is already open
// ([this+0xf4]) or the slot's user id (pad table 0x31a6bc0, 0x550 bytes per slot, +0x40) is -1.
// Its two calls are logged with those values and the result.
constexpr u64 kOpenTextEntry = 0xf34700;
constexpr u64 kOpenTextEntryCalls[] = {0xe8be5e, 0xfbff0d};
constexpr u64 kPadTable = 0x31a6bc0;
using OpenTextEntryFn = s32(PS4_SYSV_ABI*)(u8*, s32);
OpenTextEntryFn g_open_text_entry = nullptr;
u8* g_game_base = nullptr;

s32 PS4_SYSV_ABI OpenTextEntryHook(u8* self, s32 slot) {
    const u8* table = g_game_base + kPadTable + kTextRvaShift;
    s32 users[4];
    for (int i = 0; i < 4; ++i) {
        std::memcpy(&users[i], table + i * 0x550 + 0x40, 4);
    }
    s32 open_state = 0;
    std::memcpy(&open_state, self + 0xf4, 4);
    const s32 result = g_open_text_entry(self, slot);
    std::fprintf(stderr,
                 "[ime] text entry: slot %d, users %d %d %d %d, open state %d, dialog mode %d -> %d\n",
                 slot, users[0], users[1], users[2], users[3], open_state, self[0x108], result);
    return result;
}

void InstallTextEntryDiag(u8* base) {
    g_game_base = base;
    u8* stubs = AllocNear(base);
    if (!stubs) {
        return;
    }
    g_open_text_entry = reinterpret_cast<OpenTextEntryFn>(base + kOpenTextEntry + kTextRvaShift);
    for (size_t i = 0; i < std::size(kOpenTextEntryCalls); ++i) {
        RedirectCall(base, stubs + i * 16, kOpenTextEntryCalls[i], kOpenTextEntry,
                     reinterpret_cast<void*>(&OpenTextEntryHook), "DIAG-106 text entry");
    }
}

#endif

// GTS-006: custom decals offline. <exe dir>\decals\*.svg are copied to /APP_CACHE/decals (the
// game's <exe dir>\temp0\decals) as "<id>_<name>.svg"; the patched GRIM script lists them as the
// user's decals and copies each into the decal cache under that id. The id is a hash of the file
// name, so a decal keeps its id (and liveries that use it keep working) across runs.
void SyncDecals() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir{exe};
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    const std::wstring src = dir + L"decals\\";
    const std::wstring dst = dir + L"temp0\\decals\\";
    CreateDirectoryW(src.c_str(), nullptr);
    CreateDirectoryW((dir + L"temp0").c_str(), nullptr);
    CreateDirectoryW(dst.c_str(), nullptr);
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW((dst + L"*.svg").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            DeleteFileW((dst + found.cFileName).c_str());
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    find = FindFirstFileW((src + L"*.svg").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    int count = 0;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        const std::wstring file{found.cFileName};
        u32 hash = 2166136261u;
        std::string title;
        for (wchar_t c : file.substr(0, file.size() - 4)) {
            const wchar_t lower = towlower(c);
            hash = (hash ^ static_cast<u32>(lower)) * 16777619u;
            title += (c < 128 && (iswalnum(c) || c == L' ' || c == L'-' || c == L'_'))
                         ? static_cast<char>(c) : '_';
        }
        const u32 id = 2000000000u + hash % 100000000u;
        const std::string name = std::to_string(id) + "_" + title + ".svg";
        if (CopyFileW((src + file).c_str(), (dst + std::wstring(name.begin(), name.end())).c_str(),
                      FALSE)) {
            ++count;
        }
    } while (FindNextFileW(find, &found));
    FindClose(find);
    std::fprintf(stderr, "[patch] GTS-006 %d custom decal(s) from decals\\\n", count);
}

// GTS-007: wet-road reflections off. In the rain GT Sport renders the scene a second time into a
// 1024x512 reflection of the wet track (ReflectSetting.enableIfWet of the course settings, checked
// at eboot 0xab27d2): ~24% of a Tokyo rain frame's draws. Ignoring "wet" leaves the pass out
// (Tokyo rain, 3 races each: 40.6 fps vs 31.1). Off when GPU.wet_reflections is false in
// user\custom_configs\CUSA03220.json (the launcher's option) or RYGT_WET_REFLECTIONS=0.
bool WetReflectionsWanted() {
    if (const char* env = std::getenv("RYGT_WET_REFLECTIONS")) {
        return env[0] != '0';
    }
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return true;
    }
    std::string config{path, length};
    config = config.substr(0, config.find_last_of("\\/") + 1) + "user\\custom_configs\\CUSA03220.json";
    std::FILE* file = std::fopen(config.c_str(), "rb");
    if (!file) {
        return true;
    }
    std::string text;
    char chunk[4096];
    while (const size_t n = std::fread(chunk, 1, sizeof(chunk), file)) {
        text.append(chunk, n);
    }
    std::fclose(file);
    const size_t key = text.find("\"wet_reflections\"");
    if (key == std::string::npos) {
        return true;
    }
    const size_t value = text.find_first_not_of(" \t\r\n:", key + 17);
    return value == std::string::npos || text.compare(value, 5, "false") != 0;
}

// GTS-008: rear-view mirror at half rate. The frame render (eboot 0x10ff... ) renders the back
// mirror in a block it skips when the mirror object is null or the scene's mirror-off flag
// ([r13+0x5a8]) is set: "cmp byte ptr [r13+0x5a8], 0; jne skip" at eboot 0x110011a. The cmp becomes
// a call to a stub that also skips every other time, so the mirror is redrawn every second frame
// (its picture is kept in between). GPU.mirror_half_rate=true (launcher) or RYGT_MIRROR_HALF=1.
bool ConfigFlag(const char* key, bool fallback) {
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return fallback;
    }
    std::string config{path, length};
    config = config.substr(0, config.find_last_of("\\/") + 1) + "user\\custom_configs\\CUSA03220.json";
    std::FILE* file = std::fopen(config.c_str(), "rb");
    if (!file) {
        return fallback;
    }
    std::string text;
    char chunk[4096];
    while (const size_t n = std::fread(chunk, 1, sizeof(chunk), file)) {
        text.append(chunk, n);
    }
    std::fclose(file);
    const std::string quoted = std::string{"\""} + key + "\"";
    const size_t at = text.find(quoted);
    if (at == std::string::npos) {
        return fallback;
    }
    const size_t value = text.find_first_not_of(" \t\r\n:", at + quoted.size());
    if (value == std::string::npos) {
        return fallback;
    }
    return text.compare(value, 4, "true") == 0;
}

// Disable the two motion-blur inputs while retaining the game's normal effect graph.
// CUSA03220 v1.69: EffectParameter.motionblur.enable (+0x264) and
// fixmotionblur.enable (+0x288) are read when scheduling the post-process jobs.
// The combined scene-motion-blur/depth-of-field job must still run for defocus.
void InstallMotionBlurOption(u8* base) {
    if (!ConfigFlag("disable_motion_blur", false)) return;
    const Patch patches[] = {
        {0x1146c00, {0x80, 0xb8, 0x64, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x95, 0xc1},
         {0xb9, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90},
         "motion blur: scene effect disabled"},
        {0x1146c1b, {0x8a, 0x90, 0x88, 0x02, 0x00, 0x00},
         {0xb2, 0x00, 0x90, 0x90, 0x90, 0x90}, "motion blur: fixed effect disabled"},
    };
    // Check both sites before changing either; other executable versions stay untouched.
    for (const auto& patch : patches) {
        MEMORY_BASIC_INFORMATION info{};
        const auto* target = base + patch.rva;
        if (!VirtualQuery(target, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            reinterpret_cast<uintptr_t>(target) + patch.original.size() >
                reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            std::memcmp(target, patch.original.data(), patch.original.size()) != 0) {
            std::fprintf(stderr, "[patch] motion blur: unsupported executable; option not applied\n");
            return;
        }
    }
    for (const auto& patch : patches) {
        if (!Apply(base, patch)) return;
    }
}

// A setting of the GT Sport config as written (a string without its quotes, a number), or "".
static std::string ConfigValue(const char* key) {
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    std::string config{path, length};
    config = config.substr(0, config.find_last_of("\\/") + 1) + "user\\custom_configs\\CUSA03220.json";
    std::FILE* file = std::fopen(config.c_str(), "rb");
    if (!file) {
        return {};
    }
    std::string text;
    char chunk[4096];
    while (const size_t n = std::fread(chunk, 1, sizeof(chunk), file)) {
        text.append(chunk, n);
    }
    std::fclose(file);
    const std::string quoted = std::string{"\""} + key + "\"";
    const size_t at = text.find(quoted);
    if (at == std::string::npos) {
        return {};
    }
    const size_t value = text.find_first_not_of(" \t\r\n:", at + quoted.size());
    if (value == std::string::npos) {
        return {};
    }
    if (text[value] == '"') {
        const size_t end = text.find('"', value + 1);
        return end == std::string::npos ? std::string{} : text.substr(value + 1, end - value - 1);
    }
    const size_t end = text.find_first_of(",}\r\n", value);
    return text.substr(value, end == std::string::npos ? std::string::npos : end - value);
}

// GTS-009: the camera's aspect ratio for a screen wider (or narrower) than 16:9, from
// GPU.aspect_ratio: "auto" (the shape of the game's window or screen, from the display settings),
// "21:9", "32:9", another width:height or a number. 0 when the game's own 16:9 applies.
// RYGT_ASPECT overrides it.
namespace Vulkan {
float RytyDisplayAspect() {
    static const float aspect = [] {
        std::string value = ConfigValue("aspect_ratio");
        if (const char* env = std::getenv("RYGT_ASPECT")) {
            value = env;
        }
        double ratio = 0.0;
        if (value == "auto") {
            const bool full = ConfigValue("full_screen") == "true";
            const bool exclusive = ConfigValue("full_screen_mode") == "Fullscreen";
            double width = std::atof(ConfigValue("window_width").c_str());
            double height = std::atof(ConfigValue("window_height").c_str());
            if (full && !exclusive) { // borderless: the desktop's resolution
                width = GetSystemMetrics(SM_CXSCREEN);
                height = GetSystemMetrics(SM_CYSCREEN);
            }
            ratio = height > 0.0 ? width / height : 0.0;
        } else if (const size_t colon = value.find(':'); colon != std::string::npos) {
            const double height = std::atof(value.substr(colon + 1).c_str());
            ratio = height > 0.0 ? std::atof(value.substr(0, colon).c_str()) / height : 0.0;
        } else {
            ratio = std::atof(value.c_str());
        }
        if (!(ratio > 0.5 && ratio < 6.0) || std::abs(ratio - 16.0 / 9.0) < 0.01) {
            return 0.0f;
        }
        return static_cast<float>(ratio);
    }();
    return aspect;
}
} // namespace Vulkan

// GTS-009: the race camera's view is set up with a 16:9 aspect
// (eboot 0x5e717f: mov dword [rbx+0x8d464], 1.7777778f); the wider aspect widens its view.
static void InstallWideCamera(u8* base) {
    const float aspect = Vulkan::RytyDisplayAspect();
    if (aspect <= 0.0f) {
        return;
    }
    const float original = 16.0f / 9.0f;
    std::vector<u8> from(4), to(4);
    std::memcpy(from.data(), &original, 4);
    std::memcpy(to.data(), &aspect, 4);
    if (Apply(base, Patch{0x5f7185, from, to, fmt::format("GTS-009 camera aspect {:.3f}", aspect)})) {
        std::fprintf(stderr, "[patch] GTS-009: picture stretched to the window\n");
    }
}

void InstallMirrorHalfRate(u8* base) {
    bool wanted = ConfigFlag("mirror_half_rate", false);
    if (const char* env = std::getenv("RYGT_MIRROR_HALF")) {
        wanted = env[0] == '1';
    }
    if (!wanted) {
        return;
    }
    bool temporal = ConfigFlag("dlss_enabled", false);
    if (const char* overrideMode = std::getenv("SHADGT_DLSS_TEMPORAL")) {
        temporal = std::strcmp(overrideMode, "1") == 0;
    }
    // FSR-200: temporal FSR reconstructs from the same scene, so it needs the same continuity.
    bool fsrTemporal = ConfigFlag("fsr_enabled", false) && ConfigFlag("fsr3_enabled", true) &&
                       ConfigFlag("fsr_temporal", true);
    if (const char* overrideMode = std::getenv("SHADGT_FSR_TEMPORAL")) {
        fsrTemporal = overrideMode[0] == '1';
    }
    if (temporal || fsrTemporal) {
        std::fprintf(stderr, "[patch] GTS-008: half-rate mirror disabled for temporal %s motion continuity\n",
                     temporal ? "DLSS" : "FSR");
        return;
    }
    constexpr u64 kSite = 0x111011a; // RVA of "cmp byte ptr [r13+0x5a8], 0"
    static const u8 kOriginal[] = {0x41, 0x80, 0xbd, 0xa8, 0x05, 0x00, 0x00, 0x00};
    u8* site = base + kSite;
    if (std::memcmp(site, kOriginal, sizeof(kOriginal)) != 0) {
        std::fprintf(stderr, "[patch] GTS-008: original bytes differ, not applied\n");
        return;
    }
    u8* stub = AllocNear(base);
    if (!stub) {
        std::fprintf(stderr, "[patch] GTS-008: no memory near the game's code\n");
        return;
    }
    // cmp byte [r13+0x5a8],0 ; jne skip ; xor byte [rip+toggle],1 ; ret ; skip: ret ; toggle: 0
    const u8 code[] = {0x41, 0x80, 0xbd, 0xa8, 0x05, 0x00, 0x00, 0x00, // +0  cmp
                       0x75, 0x08,                                     // +8  jne +18
                       0x80, 0x35, 0x02, 0x00, 0x00, 0x00, 0x01,       // +10 xor [rip+2],1
                       0xc3,                                           // +17 ret (ZF from xor)
                       0xc3,                                           // +18 ret (ZF=0: skip)
                       0x00};                                          // +19 toggle
    std::memcpy(stub, code, sizeof(code));
    u8 call[8] = {0xe8, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const s32 rel = static_cast<s32>(reinterpret_cast<s64>(stub) - reinterpret_cast<s64>(site + 5));
    std::memcpy(call + 1, &rel, 4);
    Apply(base, Patch{kSite, {std::begin(kOriginal), std::end(kOriginal)},
                      {std::begin(call), std::end(call)}, "GTS-008 rear-view mirror at half rate"});
}

namespace {
using TelemetryUpdateFn = void(PS4_SYSV_ABI*)(void*, void*);
TelemetryUpdateFn g_telemetry_update{};
void PS4_SYSV_ABI TelemetryUpdateHook(void* service, void* context) {
    g_telemetry_update(service, context);
    const auto* packet = static_cast<const u8*>(service) + 0x1dc;
    Vulkan::RytyDlssG::PublishTelemetry(packet);
    Input::TriggerFeedback::Publish(packet);
}
void InstallTriggerTelemetry(u8* base) {
    // CUSA03220 v1.69: SimulatorInterface::update virtual slot, converter RVA.
    // Validate both the slot and packet-builder magic before touching the table.
    auto* slot = reinterpret_cast<TelemetryUpdateFn*>(base + 0x2858390);
    auto original = reinterpret_cast<TelemetryUpdateFn>(base + 0x68cf20);
    const u8 magic[] = {0xc7, 0x87, 0xdc, 0x01, 0, 0, 0x30, 0x53, 0x37, 0x47};
    if (*slot != original || std::memcmp(base + 0x68cf53, magic, sizeof(magic)) != 0) return;
    const char* enabled = std::getenv("RYGT_ADAPTIVE_TRIGGERS");
    if (enabled && enabled[0] == '0' && !ConfigFlag("dlssg_enabled", false)) return;
    DWORD protection;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) return;
    g_telemetry_update = original;
    *slot = &TelemetryUpdateHook;
    DWORD ignored;
    VirtualProtect(slot, sizeof(*slot), protection, &ignored);
}
} // namespace

namespace {
// DIAG-161: GT Sport's fatal-error routine (eboot 0x1796f70: source file, line, message) throws the
// error that ends the game: a script's System.ErrorAbort, a failed native assert. The game shows
// the message nowhere and then shuts down as if the player quit (a player's window closed right
// after the boot screen with nothing in the logs). Its entry now prints the message to stderr
// first. The message is the game's string: an 8-byte header, then the text inline (capacity under
// 16) or a pointer to it, the size at +0x18 and the capacity at +0x20.
constexpr u64 kFatalError = 0x1796f70;
constexpr u8 kFatalErrorPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                      0x56, 0x41, 0x55, 0x41, 0x54, 0x53};

void PS4_SYSV_ABI FatalErrorHook(const char* file, s32 line, const u8* message) {
    static std::atomic<u32> reported{0};
    if (reported++ >= 20) {
        return;
    }
    char where[256] = "?";
    if (file) {
        for (size_t i = 0; i + 1 < sizeof(where) && Core::RytyMemory::GuardedCopy(where + i, file + i, 1); ++i) {
            if (where[i] == 0) {
                break;
            }
            where[i + 1] = 0;
        }
    }
    char text[2048] = "";
    u64 header[5] = {};
    if (message && Core::RytyMemory::GuardedCopy(header, message, sizeof(header))) {
        const u64 size = std::min<u64>(header[3], sizeof(text) - 1);
        const char* data = header[4] >= 0x10 ? reinterpret_cast<const char*>(header[1])
                                             : reinterpret_cast<const char*>(message + 8);
        if (size != 0 && Core::RytyMemory::GuardedCopy(text, data, size)) {
            text[size] = 0;
        }
    }
    std::fprintf(stderr, "[game] fatal error at %s(%d): %s\n", where, line, text);
    std::fflush(stderr);
}

void InstallFatalErrorDiag(u8* base) {
    u8* entry = base + kFatalError + kTextRvaShift;
    if (std::memcmp(entry, kFatalErrorPrologue, sizeof(kFatalErrorPrologue)) != 0) {
        std::fprintf(stderr, "[patch] DIAG-161: original bytes differ, not applied\n");
        return;
    }
    u8* stub = AllocNear(base);
    if (!stub) {
        std::fprintf(stderr, "[patch] DIAG-161: no memory near the game's code\n");
        return;
    }
    // Saves the argument registers, calls the hook with the stack aligned, restores them, runs the
    // 13 prologue bytes the jump replaced and jumps back after them.
    std::vector<u8> code = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, // push rdi..r9
                            0x48, 0x83, 0xec, 0x08,                         // sub rsp, 8
                            0x48, 0xb8};                                    // mov rax, imm64
    const u64 hook = reinterpret_cast<u64>(&FatalErrorHook);
    code.insert(code.end(), reinterpret_cast<const u8*>(&hook), reinterpret_cast<const u8*>(&hook) + 8);
    code.insert(code.end(), {0xff, 0xd0,                                    // call rax
                             0x48, 0x83, 0xc4, 0x08,                        // add rsp, 8
                             0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f}); // pop r9..rdi
    code.insert(code.end(), std::begin(kFatalErrorPrologue), std::end(kFatalErrorPrologue));
    code.insert(code.end(), {0xff, 0x25, 0, 0, 0, 0});                      // jmp [rip]
    const u64 back = reinterpret_cast<u64>(entry + sizeof(kFatalErrorPrologue));
    code.insert(code.end(), reinterpret_cast<const u8*>(&back), reinterpret_cast<const u8*>(&back) + 8);
    std::memcpy(stub, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), stub, code.size());
    u8 jump[sizeof(kFatalErrorPrologue)];
    std::memset(jump, 0x90, sizeof(jump));
    jump[0] = 0xe9;
    const s32 rel = static_cast<s32>(stub - (entry + 5));
    std::memcpy(jump + 1, &rel, 4);
    DWORD old_protect = 0;
    if (!VirtualProtect(entry, sizeof(jump), PAGE_EXECUTE_READWRITE, &old_protect)) {
        return;
    }
    std::memcpy(entry, jump, sizeof(jump));
    DWORD ignored = 0;
    VirtualProtect(entry, sizeof(jump), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(jump));
    std::fprintf(stderr, "[patch] DIAG-161 fatal-error message applied at %#llx\n",
                 static_cast<unsigned long long>(kFatalError));
}
} // namespace

// GTS-009 (UI-HIRES): the game's 4K UI images. The UI resolution flag ([0x143056f29], "4K output
// and not the override at [0x143056f2b]") is set at its two writers instead, so the menus and the
// HUD load their _4K packs (twice the image detail) while the game renders at 1080 Pro. Drawn at
// the window size by UI-HIRES, they are sharp at 1440 and above. With GPU.ui_hires or
// RYGT_UI_4K_IMAGES=1 (=0 off).
void InstallUi4kImages(u8* base) {
    bool wanted = ConfigFlag("ui_hires", false);
    if (const char* env = std::getenv("RYGT_UI_4K_IMAGES")) {
        wanted = env[0] == '1';
    }
    if (!wanted) {
        return;
    }
    // and al, dl -> mov al, 1; and cl, dl -> mov cl, 1
    Apply(base, Patch{0xe548e2, {0x20, 0xd0}, {0xb0, 0x01}, "GTS-009 4K UI images (1/2)"});
    Apply(base, Patch{0xf387b2, {0x20, 0xd1}, {0xb1, 0x01}, "GTS-009 4K UI images (2/2)"});
}

// The page handoff closes the keyboard even when no dialog was opened.
// Calling the original close in state zero waits 300 yields before cleanup.
using OskCloseFn = void(PS4_SYSV_ABI*)(u64);
OskCloseFn g_osk_close{};

void PS4_SYSV_ABI CloseOskIfActive(u64 keyboard) {
    u32 state{};
    if (Core::RytyMemory::GuardedCopy(&state,
            reinterpret_cast<const void*>(keyboard + 0xf4), sizeof(state)) && state == 0) {
        return;
    }
    // Preserve active, pending and error-state shutdown, including unreadable state.
    g_osk_close(keyboard);
}

void InstallIdleKeyboardClose(u8* base) {
    Native::NearCode code{base};
    constexpr std::array<u8, 6> prologue{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    g_osk_close = reinterpret_cast<OskCloseFn>(Native::HookEntry(
        code, base, 0xf345a0, prologue, reinterpret_cast<void*>(&CloseOskIfActive),
        "idle keyboard close"));
}

// Menu CPU snapshots are unsafe during every native race session, including
// attract replays which can reuse scripts without updating the project hint.
using SnapshotSessionFn = void(PS4_SYSV_ABI*)(u64, u64, s32, u64);
Native::MenuSnapshotSession g_snapshot_session;
SnapshotSessionFn g_snapshot_start{}, g_snapshot_end{};
bool g_snapshot_session_ready = false; // Installed before game threads start.
void PS4_SYSV_ABI SnapshotStart(u64 result, u64 self, s32 argc, u64 argv) {
    g_snapshot_session.Begin();
    g_snapshot_start(result, self, argc, argv);
}
void PS4_SYSV_ABI SnapshotEnd(u64 result, u64 self, s32 argc, u64 argv) {
    const auto token = g_snapshot_session.Ending();
    g_snapshot_end(result, self, argc, argv);
    g_snapshot_session.End(token);
}
void InstallMenuSnapshotSessionGuard(u8* base) {
    Native::NearCode code{base};
    constexpr u8 prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    g_snapshot_start = reinterpret_cast<SnapshotSessionFn>(Native::HookEntry(
        code, base, 0x3e1900, prologue, reinterpret_cast<void*>(&SnapshotStart), "menu snapshot session start"));
    g_snapshot_end = reinterpret_cast<SnapshotSessionFn>(Native::HookEntry(
        code, base, 0x3e1b00, prologue, reinterpret_cast<void*>(&SnapshotEnd), "menu snapshot session end"));
    g_snapshot_session_ready = g_snapshot_start && g_snapshot_end;
}

// The icon capture script calls Begin before scheduling any image/mask work and
// End in its cleanup. End alone is not completion: the PNG save runs on a job.
// Keep every producer required until that job has returned with terminal state 12.
// Unknown/aborted captures retain the previous conservative menu policy.
using IconCaptureFn = void(PS4_SYSV_ABI*)(u64);
Native::MenuCaptureCompileGuard g_capture_compiles;
IconCaptureFn g_icon_begin{}, g_icon_end{}, g_icon_save{};
bool g_scoped_menu_compiles = false; // Set before game threads start.
u64 g_icon_capture_vtable{};
u64 g_icon_owner_vtable{};
std::mutex g_icon_lifecycle_mutex;

bool RetainIconCapture(u64 owner) {
    u64 vtable{};
    u32 strong{};
    if (!owner || !Core::RytyMemory::GuardedCopy(&vtable, reinterpret_cast<void*>(owner), sizeof(vtable)) ||
        vtable != g_icon_owner_vtable ||
        !Core::RytyMemory::GuardedCopy(&strong, reinterpret_cast<void*>(owner + 8), sizeof(strong)) ||
        strong == 0 || strong >= (1u << 30)) return false;
    // The controller owns this shared block throughout its Begin callback.
    std::atomic_ref<u32>{*reinterpret_cast<u32*>(owner + 8)}.fetch_add(1, std::memory_order_seq_cst);
    return true;
}

void ReleaseIconCapture(u64 owner) {
    // Matches the verified game's shared-owner release sequence. The retained
    // strong reference keeps both the object and this control block alive.
    const auto* vtable = *reinterpret_cast<const u64* const*>(owner);
    if (std::atomic_ref<u32>{*reinterpret_cast<u32*>(owner + 8)}.fetch_sub(1, std::memory_order_seq_cst) == 1) {
        reinterpret_cast<IconCaptureFn>(vtable[0])(owner);
        if (std::atomic_ref<u32>{*reinterpret_cast<u32*>(owner + 12)}.fetch_sub(1, std::memory_order_seq_cst) == 1)
            reinterpret_cast<IconCaptureFn>(vtable[1])(owner);
    }
}

void PS4_SYSV_ABI IconCaptureBegin(u64 controller) {
    // End and a replacing Begin must not drop the controller's shared owner
    // between copying the pair and taking our reference. Save completion does
    // not acquire this mutex, so a waiting End cannot block the save worker.
    std::scoped_lock lifecycle_lock{g_icon_lifecycle_mutex};
    const bool tracked = g_capture_compiles.Begin(reinterpret_cast<void*>(controller));
    g_icon_begin(controller);
    std::array<u64, 2> capture{};
    u64 vtable{};
    bool pinned = false;
    if (tracked && Core::RytyMemory::GuardedCopy(capture.data(),
            reinterpret_cast<void*>(controller + 0x2950), sizeof(capture)) && capture[0] &&
        Core::RytyMemory::GuardedCopy(&vtable, reinterpret_cast<void*>(capture[0]), sizeof(vtable)) &&
        vtable == g_icon_capture_vtable) {
        pinned = RetainIconCapture(capture[1]);
    }
    if (tracked && !g_capture_compiles.Attach(reinterpret_cast<void*>(controller),
            pinned ? reinterpret_cast<void*>(capture[0]) : nullptr,
            pinned ? reinterpret_cast<void*>(capture[1]) : nullptr) && pinned) {
        ReleaseIconCapture(capture[1]);
    }
#ifndef NDEBUG
    std::fprintf(stderr, "[capture-compile] begin controller=%llx object=%llx retained=%d\n",
                 controller, capture[0], pinned);
#endif
}

void PS4_SYSV_ABI IconCaptureSave(u64 object) {
    const auto token = g_capture_compiles.StartSave(reinterpret_cast<void*>(object));
    g_icon_save(object);
    // A tracked Begin retained an engine owner. Even an early/cancelled End
    // cannot destroy it underneath this final read. Early worker returns do
    // not write state 12 and must keep the conservative policy.
    u32 state{};
    const bool saved = token && Core::RytyMemory::GuardedCopy(&state,
        reinterpret_cast<void*>(object + 0x3f8), sizeof(state)) && state == 12;
    g_capture_compiles.CompleteSave(reinterpret_cast<void*>(object), token, saved);
#ifndef NDEBUG
    std::fprintf(stderr, "[capture-compile] save object=%llx generation=%llu state=%u\n", object, token, state);
#endif
}

void PS4_SYSV_ABI IconCaptureEnd(u64 controller) {
    std::scoped_lock lifecycle_lock{g_icon_lifecycle_mutex};
    // The game can observe state 12 just before the save wrapper publishes its
    // completion. Keep its owner alive until that publication; never wait forever
    // if this is an interrupted capture or an unsupported scheduling path.
    const bool saved = g_capture_compiles.WaitForSave(reinterpret_cast<void*>(controller),
                                                     std::chrono::milliseconds{250});
    std::array<u64, 2> capture{};
    if (!Core::RytyMemory::GuardedCopy(capture.data(),
            reinterpret_cast<void*>(controller + 0x2950), sizeof(capture)) || !capture[0] || !capture[1])
        g_capture_compiles.Attach(nullptr, nullptr); // Uncertain owner: retain the pin and wait policy.
    g_icon_end(controller);
    if (g_capture_compiles.End(reinterpret_cast<void*>(controller), reinterpret_cast<void*>(capture[0]),
                              reinterpret_cast<void*>(capture[1])))
        ReleaseIconCapture(capture[1]);
#ifndef NDEBUG
    std::fprintf(stderr, "[capture-compile] end controller=%llx saved=%d continuing=%d\n",
                 controller, saved, g_capture_compiles.CanContinue());
#else
    (void)saved;
#endif
}

void InstallScopedMenuCompiles(u8* base) {
    const char* enabled = std::getenv("RYGT_CAPTURE_SCOPED_MENUS");
    if (enabled && std::strcmp(enabled, "0") == 0) return;
    // Verify implementation-specific fields and virtual entries, not just the
    // common function prologue. Unsupported game versions retain menu waits.
    const auto matches = [&](u64 rva, std::span<const u8> expected) {
        std::array<u8, 64> actual{};
        return expected.size() <= actual.size() &&
               Core::RytyMemory::GuardedCopy(actual.data(), base + rva, expected.size()) &&
               std::equal(expected.begin(), expected.end(), actual.begin());
    };
    constexpr u8 begin_field[]{0x48,0x8d,0xb7,0x6c,0x29,0,0};
    constexpr u8 end_field[]{0xc7,0x87,0x68,0x29,0,0,3,0,0,0};
    constexpr u8 save_field[]{0x48,0x8b,0x87,0x80,3,0,0,0x48,0x3b,0x87,0x88,3,0,0};
    constexpr u8 terminal[]{0x41,0xc7,0x84,0x24,0xf8,3,0,0,12,0,0,0};
    constexpr u8 prologue[]{0x55,0x48,0x89,0xe5,0x41,0x57};
    u64 save_entry{};
    std::array<u64, 2> owner_entries{};
    if (!matches(0x5652b0, prologue) || !matches(0x5651e0, prologue) ||
        !matches(0x56c7e0, prologue) || !matches(0x5652cb, begin_field) ||
        !matches(0x565202, end_field) || !matches(0x56c802, save_field) ||
        !matches(0x56d69e, terminal) ||
        !Core::RytyMemory::GuardedCopy(&save_entry, base + 0x28443d0 + 0x40, sizeof(save_entry)) ||
        save_entry != reinterpret_cast<u64>(base + 0x56c400) ||
        !Core::RytyMemory::GuardedCopy(owner_entries.data(), base + 0x2844398, sizeof(owner_entries)) ||
        owner_entries[0] != reinterpret_cast<u64>(base + 0x56e4b0) ||
        owner_entries[1] != reinterpret_cast<u64>(base + 0x56e4c0)) {
        g_capture_compiles.ConfigureReady(false);
        std::fprintf(stderr, "[capture-compile] unsupported capture implementation; retaining menu waits\n");
        return;
    }
    Native::NearCode code{base};
    g_icon_begin = reinterpret_cast<IconCaptureFn>(Native::HookEntry(code, base, 0x5552b0,
        prologue, reinterpret_cast<void*>(&IconCaptureBegin), "icon capture compile begin"));
    g_icon_end = reinterpret_cast<IconCaptureFn>(Native::HookEntry(code, base, 0x5551e0,
        prologue, reinterpret_cast<void*>(&IconCaptureEnd), "icon capture compile end"));
    g_icon_save = reinterpret_cast<IconCaptureFn>(Native::HookEntry(code, base, 0x55c7e0,
        prologue, reinterpret_cast<void*>(&IconCaptureSave), "icon capture save completion"));
    g_icon_capture_vtable = reinterpret_cast<u64>(base + 0x28443d0);
    g_icon_owner_vtable = reinterpret_cast<u64>(base + 0x2844398);
    g_scoped_menu_compiles = g_icon_begin && g_icon_end && g_icon_save;
    g_capture_compiles.ConfigureReady(g_scoped_menu_compiles);
    std::fprintf(stderr, "[capture-compile] scoped menu compiles %s\n",
                 g_scoped_menu_compiles ? "ready" : "unavailable; retaining menu waits");
}

// Diagnostic comparison: keep output/UI at 1080p while the allocator experiment
// supplies 540p scene targets. This scoped helper already supports separate scene
// scale inputs; its default 1:1 scale otherwise expands scene logical viewports.
using SceneViewportFn = void(PS4_SYSV_ABI*)(u64, u64, u64, u64, u64, u64,
                                         float, float, float, float, float, float, u64, u64);
SceneViewportFn g_scene_viewport{};
void PS4_SYSV_ABI MatchExperimentalSceneViewport(
    u64 self, u64 output, u64 auxiliary, u64 scene, u64 depth, u64 motion,
    float x, float y, float width, float height, float scene_x, float scene_y,
    u64 extra_a, u64 extra_b) {
    static const u32 scene_width = [] {
        const char* value = std::getenv("RYGT_SCENE_WIDTH");
        return value ? u32(std::strtoul(value, nullptr, 10)) : 960u;
    }();
    static const u32 scene_height = [] {
        const char* value = std::getenv("RYGT_SCENE_HEIGHT");
        return value ? u32(std::strtoul(value, nullptr, 10)) : 540u;
    }();
    u32 output_extent{}, scene_extent{};
    // These are verified color-target hint fields, not guessed ImageInfo offsets.
    if (output && scene && scene_x == 1.0f && scene_y == 1.0f &&
        Core::RytyMemory::GuardedCopy(&output_extent,
            reinterpret_cast<const void*>(output + 0x44), sizeof(output_extent)) &&
        Core::RytyMemory::GuardedCopy(&scene_extent,
            reinterpret_cast<const void*>(scene + 0x44), sizeof(scene_extent)) &&
        output_extent == 0x04380780 && scene_width >= 320 && scene_width <= 3840 &&
        scene_height >= 180 && scene_height <= 2160 &&
        scene_extent == ((scene_height << 16) | scene_width)) {
        scene_x = float(scene_width) / 1920.f;
        scene_y = float(scene_height) / 1080.f;
        static std::atomic<u32> logged{};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 4) {
            std::fprintf(stderr, "[resolution] scene viewport %ux%u; game composition 1920x1080\n", scene_width, scene_height);
        }
    }
    g_scene_viewport(self, output, auxiliary, scene, depth, motion,
                     x, y, width, height, scene_x, scene_y, extra_a, extra_b);
}
// GTSport's history builder scales its region dimensions but keeps the output
// origin in 1080p coordinates. Scale those local X/Y values before history writes
// and copy UVs are generated; leave the output object's UI placement unchanged.
void InstallSceneHistoryOrigins(u8* base) {
    const auto read_size = [](const char* name, u32 fallback) {
        const char* value = std::getenv(name);
        return value ? u32(std::strtoul(value, nullptr, 10)) : fallback;
    };
    const u32 width = read_size("RYGT_SCENE_WIDTH", 960);
    const u32 height = read_size("RYGT_SCENE_HEIGHT", 540);
    if (width < 320 || width > 3840 || height < 180 || height > 2160 ||
        (width == 1920 && height == 1080)) return;
    const std::array<u8, 6> original_x{0x48,0x8b,0x03,0x48,0x89,0xdf};
    const std::array<u8, 8> original_y{0x48,0x89,0x84,0x24,0xb0,0,0,0};
    constexpr u64 x_rva = Native::kEbootToExe + 0x197f3bc;
    constexpr u64 y_rva = Native::kEbootToExe + 0x197f3cc;
    if (std::memcmp(base + x_rva, original_x.data(), original_x.size()) ||
        std::memcmp(base + y_rva, original_y.data(), original_y.size())) {
        std::fprintf(stderr, "[resolution] history origin signatures differ; not applied\n");
        return;
    }
    Native::NearCode origin_code{base};
    const auto prepare = [&](u64 rva, std::span<const u8> original, float scale, bool y) {
        auto bytes = Native::SceneHistoryOriginCode((height << 16) | width, scale, y);
        bytes.insert(bytes.end(), original.begin(), original.end());
        bytes.insert(bytes.end(), {0xff,0x25,0,0,0,0});
        const u64 back = reinterpret_cast<u64>(base + rva + original.size());
        const auto* address = reinterpret_cast<const u8*>(&back);
        bytes.insert(bytes.end(), address, address + 8);
        u8* stub = origin_code.Alloc(bytes.size());
        if (!stub) return std::vector<u8>{};
        std::memcpy(stub, bytes.data(), bytes.size());
        FlushInstructionCache(GetCurrentProcess(), stub, bytes.size());
        const s64 distance = stub - (base + rva + 5);
        if (distance != s32(distance)) return std::vector<u8>{};
        std::vector<u8> jump(original.size(), 0x90); jump[0] = 0xe9;
        const s32 relative = s32(distance); std::memcpy(jump.data() + 1, &relative, 4);
        return jump;
    };
    const auto x = prepare(x_rva, original_x, float(width) / 1920.f, false);
    const auto y = prepare(y_rva, original_y, float(height) / 1080.f, true);
    if (x.empty() || y.empty()) return;
    if (!Apply(base, {x_rva, {original_x.begin(),original_x.end()}, x, "scene history X origin"})) return;
    if (!Apply(base, {y_rva, {original_y.begin(),original_y.end()}, y, "scene history Y origin"}))
        Apply(base, {x_rva, x, {original_x.begin(),original_x.end()}, "restore history X origin"});
}
void InstallExperimentalSceneViewport(u8* base) {
    const char* enabled = std::getenv("RYGT_EXPERIMENTAL_SCENE_VIEWPORT");
    if (!enabled || std::strcmp(enabled, "1") != 0) return;
    InstallSceneHistoryOrigins(base);
    Native::NearCode code{base};
    constexpr std::array<u8, 6> original{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    g_scene_viewport = reinterpret_cast<SceneViewportFn>(Native::HookEntry(
        code, base, 0x18d2cb0, original,
        reinterpret_cast<void*>(&MatchExperimentalSceneViewport), "experimental scene viewport scale"));
}

// PERF-GT7-113: GT7's file cipher. The game decrypts what it reads (ChaCha20: 20 rounds, a 32-bit
// block counter in state word 12, 16 state words at +8 of the cipher object) one 64-byte block at
// a time and XORs the keystream one byte at a time: while a race loads its file thread (FexpR)
// spends ~65% of its time there (1.44 s frame after Enter on Monza: ~0.9 s of it). The same
// cipher here makes 8 blocks at once (AVX2, or 4 with SSE) and XORs 8 bytes at a time, with the
// same output and the same counter left in the object. Hooked where the default and the custom
// seek paths join (the three instructions moved to the stub are position independent), so a
// cipher object with its own block-step method still runs the game's code. Applied only when the
// function's bytes are the expected ones and the self-test passes. RYGT_FAST_CIPHER=0: off.
// RYGT_FAST_CIPHER_VERIFY=<n>: the first n calls also run the game's own code on a copy of the
// data and compare. RYGT_DIAG_CIPHER=1: calls, MB and time a second on stderr.
namespace Gt7Cipher {

constexpr u64 kXor = 0x3326e10;         // void xor(Cipher* self, u8* data, s64 size, u64 offset)
constexpr u64 kLoop = 0x3326f4b;        // keystream merge and the byte loop
constexpr u64 kJoin = 0x3326e50;        // the block computation, after the seek
constexpr u64 kResume = 0x3326e5f;      // after the three loads moved to the stub
constexpr u64 kEpilogue = 0x3327092;    // stack check and return
constexpr u64 kDefaultSeek = 0x2bec400; // vtable +0x8 that the code inlines
constexpr u64 kDefaultNext = 0x2bec410; // vtable +0x10 that the code inlines

constexpr u8 kEntryBytes[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83,
    0xec, 0x48, 0x4c, 0x8b, 0x35, 0xb8, 0x7e, 0x24, 0x02, 0x4c, 0x8d, 0x0d, 0xd1, 0x55, 0x8c,
    0xff, 0x49, 0x8b, 0x06, 0x48, 0x89, 0x45, 0xd0, 0x48, 0x89, 0xc8, 0x4c, 0x8b, 0x07, 0x48,
    0xc1, 0xe8, 0x06, 0x4d, 0x8b, 0x40, 0x08, 0x4d, 0x39, 0xc8, 0x0f, 0x85, 0xa1, 0x02, 0x00,
    0x00, 0x89, 0x47, 0x38, 0xc5, 0xfa, 0x6f, 0x47, 0x08, 0xc5, 0xfa, 0x6f, 0x4f, 0x18, 0xc5,
    0xfa, 0x6f, 0x57, 0x28, 0xc5, 0xfa, 0x6f, 0x5f, 0x38, 0xc5, 0x7a, 0x6f, 0x0d, 0xa4, 0xb4,
    0xc1, 0x00, 0xc5, 0x79, 0x6f, 0x15, 0x4c, 0x53, 0xc2, 0x00};
constexpr u8 kLoopBytes[] = {
    0xc5, 0xd9, 0xfe, 0xc0, 0x48, 0x85, 0xd2, 0xc5, 0xc9, 0xfe, 0xc9, 0xc5, 0xd1, 0xfe, 0xd2,
    0xc5, 0xc1, 0xfe, 0xdb, 0xc5, 0xfa, 0x7f, 0x45, 0x90, 0xc5, 0xfa, 0x7f, 0x4d, 0xa0, 0xc5,
    0xfa, 0x7f, 0x55, 0xb0, 0xc5, 0xfa, 0x7f, 0x5d, 0xc0, 0x0f, 0x8e, 0x1a, 0x01, 0x00, 0x00,
    0x48, 0x01, 0xf2, 0x83, 0xe1, 0x3f, 0x4c, 0x8d, 0x3d, 0x8b, 0x54, 0x8c, 0xff, 0x8a, 0x44,
    0x0d, 0x90, 0x30, 0x06, 0x48, 0x83, 0xf9, 0x3f, 0x0f, 0x83, 0x15, 0x01, 0x00, 0x00, 0x48,
    0xff, 0xc1, 0x48, 0xff, 0xc6, 0x48, 0x39, 0xd6, 0x72, 0xe5, 0xe9, 0xed, 0x00, 0x00, 0x00};
constexpr u8 kTailBytes[] = {
    0x49, 0x8b, 0x06, 0x48, 0x3b, 0x45, 0xd0, 0x75, 0x4c, 0x48, 0x83, 0xc4, 0x48, 0x5b, 0x41,
    0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3, 0x48, 0x8b, 0x07, 0x48, 0x8b, 0x40,
    0x10, 0x4c, 0x39, 0xf8, 0x75, 0x5b, 0xff, 0x47, 0x38, 0xc5, 0xfa, 0x6f, 0x47, 0x08, 0xc5,
    0xfa, 0x6f, 0x4f, 0x18, 0xc5, 0xfa, 0x6f, 0x57, 0x28, 0xc5, 0xfa, 0x6f, 0x5f, 0x38, 0xb8,
    0x0a, 0x00, 0x00, 0x00, 0xc5, 0xf9, 0x6f, 0xfb, 0xc5, 0xf9, 0x6f, 0xea, 0xc5, 0xf9, 0x6f,
    0xf1, 0xc5, 0xf9, 0x6f, 0xe0, 0xe9, 0xbe, 0xfe, 0xff, 0xff, 0xe8, 0xb4, 0x18, 0xa3, 0x00,
    0x0f, 0x0b};

using XorFn = void(PS4_SYSV_ABI*)(void*, u8*, s64, u64);
XorFn g_original = nullptr;
u64 g_default_seek = 0;
bool g_avx2 = false;
bool g_stats = false;
std::atomic<s64> g_verify_left{0};
std::atomic<u64> g_verify_same{0}, g_verify_differ{0}, g_verify_ns_original{0},
    g_verify_ns_fast{0}, g_verify_bytes{0};
thread_local bool t_in_original = false;
std::atomic<u64> g_calls{0}, g_bytes{0}, g_ns{0}, g_last_report{0};

#define RYGT_CHACHA_QR(a, b, c, d)                                                                 \
    a += b;                                                                                        \
    d ^= a;                                                                                        \
    d = (d << 16) | (d >> 16);                                                                     \
    c += d;                                                                                        \
    b ^= c;                                                                                        \
    b = (b << 12) | (b >> 20);                                                                     \
    a += b;                                                                                        \
    d ^= a;                                                                                        \
    d = (d << 8) | (d >> 24);                                                                      \
    c += d;                                                                                        \
    b ^= c;                                                                                        \
    b = (b << 7) | (b >> 25);

#define RYGT_CHACHA_ROUNDS(x)                                                                      \
    for (int round = 0; round < 10; ++round) {                                                     \
        RYGT_CHACHA_QR(x[0], x[4], x[8], x[12])                                                    \
        RYGT_CHACHA_QR(x[1], x[5], x[9], x[13])                                                    \
        RYGT_CHACHA_QR(x[2], x[6], x[10], x[14])                                                   \
        RYGT_CHACHA_QR(x[3], x[7], x[11], x[15])                                                   \
        RYGT_CHACHA_QR(x[0], x[5], x[10], x[15])                                                   \
        RYGT_CHACHA_QR(x[1], x[6], x[11], x[12])                                                   \
        RYGT_CHACHA_QR(x[2], x[7], x[8], x[13])                                                    \
        RYGT_CHACHA_QR(x[3], x[4], x[9], x[14])                                                    \
    }

// One block: the 16 words of the object with `counter` in word 12.
void Block(const u32* state, u32 counter, u8* out) {
    u32 s[16];
    std::memcpy(s, state, sizeof(s));
    s[12] = counter;
    u32 x[16];
    std::memcpy(x, s, sizeof(x));
    RYGT_CHACHA_ROUNDS(x)
    for (int j = 0; j < 16; ++j) {
        x[j] += s[j];
    }
    std::memcpy(out, x, 64);
}

// N blocks with counters counter..counter+N-1 (32-bit wrap, like the game), one vector lane each.
template <int N>
[[gnu::always_inline]] inline void Blocks(const u32* state, u32 counter, u8* out) {
    typedef u32 V __attribute__((vector_size(N * 4)));
    V s[16], x[16];
    for (int j = 0; j < 16; ++j) {
        for (int lane = 0; lane < N; ++lane) {
            s[j][lane] = state[j];
        }
    }
    for (int lane = 0; lane < N; ++lane) {
        s[12][lane] = counter + static_cast<u32>(lane);
    }
    for (int j = 0; j < 16; ++j) {
        x[j] = s[j];
    }
    RYGT_CHACHA_ROUNDS(x)
    u32 words[16][N];
    for (int j = 0; j < 16; ++j) {
        x[j] += s[j];
        std::memcpy(words[j], &x[j], sizeof(words[j]));
    }
    u32* out32 = reinterpret_cast<u32*>(out);
    for (int lane = 0; lane < N; ++lane) {
        for (int j = 0; j < 16; ++j) {
            out32[lane * 16 + j] = words[j][lane];
        }
    }
}

[[gnu::always_inline]] inline void XorBytes(u8* data, const u8* key, size_t size) {
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        u64 a, b;
        std::memcpy(&a, data + i, 8);
        std::memcpy(&b, key + i, 8);
        a ^= b;
        std::memcpy(data + i, &a, 8);
    }
    for (; i < size; ++i) {
        data[i] ^= key[i];
    }
}

// Whole groups of 8 (AVX2) or 4 blocks from `p`; returns the counter after them.
__attribute__((target("avx2"))) u32 Wide8(const u32* state, u32 counter, u8*& p, u8* end) {
    alignas(64) u8 key[512];
    while (end - p >= 512) {
        Blocks<8>(state, counter, key);
        XorBytes(p, key, 512);
        counter += 8;
        p += 512;
    }
    return counter;
}

u32 Wide4(const u32* state, u32 counter, u8*& p, u8* end) {
    alignas(64) u8 key[256];
    while (end - p >= 256) {
        Blocks<4>(state, counter, key);
        XorBytes(p, key, 256);
        counter += 4;
        p += 256;
    }
    return counter;
}

// The game's function from its join point: word 12 already holds the block of `offset`.
void Fast(u8* self, u8* data, s64 size, u64 offset) {
    if (size <= 0) {
        return;
    }
    u32* state = reinterpret_cast<u32*>(self + 8);
    u32 counter = state[12];
    alignas(64) u8 key[64];
    u8* p = data;
    u8* const end = data + size;
    if (const u32 pos = static_cast<u32>(offset & 63)) {
        Block(state, counter, key);
        const size_t n = std::min<size_t>(64 - pos, static_cast<size_t>(end - p));
        XorBytes(p, key + pos, n);
        p += n;
        if (pos + n == 64) {
            ++counter;
        }
    }
    counter = g_avx2 ? Wide8(state, counter, p, end) : Wide4(state, counter, p, end);
    while (end - p >= 64) {
        Block(state, counter, key);
        XorBytes(p, key, 64);
        ++counter;
        p += 64;
    }
    if (p < end) {
        Block(state, counter, key);
        XorBytes(p, key, static_cast<size_t>(end - p));
    }
    state[12] = counter;
}

u64 NowNs() {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return static_cast<u64>(static_cast<double>(now.QuadPart) * 1e9 /
                            static_cast<double>(frequency.QuadPart));
}

void Report() {
    const u64 now = NowNs();
    u64 last = g_last_report.load(std::memory_order_relaxed);
    if (last == 0) {
        g_last_report.compare_exchange_strong(last, now);
        return;
    }
    if (now - last < 1000000000ULL || !g_last_report.compare_exchange_strong(last, now)) {
        return;
    }
    const u64 calls = g_calls.exchange(0), bytes = g_bytes.exchange(0), ns = g_ns.exchange(0);
    if (calls) {
        std::fprintf(stderr, "[cipher] PERF-GT7-113: %llu calls, %.1f MB, %.1f ms (%.0f MB/s)\n",
                     static_cast<unsigned long long>(calls), bytes / 1048576.0, ns / 1e6,
                     ns ? bytes / 1048576.0 / (ns / 1e9) : 0.0);
    }
}

// Called by the stub; false: run the game's own code (the stub's fallback).
bool PS4_SYSV_ABI Hook(u8* self, u8* data, s64 size, u64 offset) {
    if (t_in_original) {
        return false;
    }
    if (g_verify_left.load(std::memory_order_relaxed) > 0 && size > 0 &&
        *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(self) + 8) ==
            g_default_seek &&
        g_verify_left.fetch_sub(1) > 0) {
        u32* state = reinterpret_cast<u32*>(self + 8);
        const u32 counter = state[12];
        std::vector<u8> copy(data, data + size);
        const u64 t0 = NowNs();
        t_in_original = true;
        g_original(self, copy.data(), size, offset);
        t_in_original = false;
        const u64 t1 = NowNs();
        const u32 counter_after = state[12];
        state[12] = counter;
        Fast(self, data, size, offset);
        const u64 t2 = NowNs();
        g_verify_ns_original += t1 - t0;
        g_verify_ns_fast += t2 - t1;
        g_verify_bytes += static_cast<u64>(size);
        const bool same = std::memcmp(copy.data(), data, static_cast<size_t>(size)) == 0 &&
                          state[12] == counter_after;
        if (same) {
            ++g_verify_same;
        } else {
            if (++g_verify_differ <= 10) {
                std::fprintf(stderr,
                             "[cipher] PERF-GT7-113: differs from the game's code (size %lld, "
                             "offset %#llx, counter %u vs %u)\n",
                             static_cast<long long>(size), static_cast<unsigned long long>(offset),
                             state[12], counter_after);
            }
            std::memcpy(data, copy.data(), static_cast<size_t>(size)); // the game's result
            state[12] = counter_after;
        }
        if (g_verify_left.load(std::memory_order_relaxed) <= 0) {
            static std::atomic<bool> told{false};
            if (!told.exchange(true)) {
                std::fprintf(stderr,
                             "[cipher] PERF-GT7-113: verified %llu calls (%.1f MB): %llu same, "
                             "%llu different; game's code %.1f ms, this one %.1f ms\n",
                             static_cast<unsigned long long>(g_verify_same + g_verify_differ),
                             g_verify_bytes / 1048576.0,
                             static_cast<unsigned long long>(g_verify_same.load()),
                             static_cast<unsigned long long>(g_verify_differ.load()),
                             g_verify_ns_original / 1e6, g_verify_ns_fast / 1e6);
            }
        }
        return true;
    }
    if (!g_stats) {
        Fast(self, data, size, offset);
        return true;
    }
    const u64 t0 = NowNs();
    Fast(self, data, size, offset);
    g_ns += NowNs() - t0;
    ++g_calls;
    g_bytes += size > 0 ? static_cast<u64>(size) : 0;
    Report();
    return true;
}

bool SelfTest() {
    // RFC 8439 2.3.2: key 00..1f, nonce 00000009 0000004a 00000000, block count 1.
    const u32 rfc[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574, 0x03020100, 0x07060504,
                         0x0b0a0908, 0x0f0e0d0c, 0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c,
                         0x00000001, 0x09000000, 0x4a000000, 0x00000000};
    const u8 expected[16] = {0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
                             0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4};
    u8 out[64];
    Block(rfc, 1, out);
    if (std::memcmp(out, expected, sizeof(expected)) != 0) {
        return false;
    }
    // The wide paths against single blocks, across a counter wrap.
    u32 state[16];
    for (int j = 0; j < 16; ++j) {
        state[j] = 0x9e3779b9u * static_cast<u32>(j + 1);
    }
    alignas(64) u8 one[64 * 8], wide[64 * 8];
    const u32 start = 0xfffffffdu;
    for (u32 b = 0; b < 8; ++b) {
        Block(state, start + b, one + b * 64);
    }
    u8 data[64 * 8 + 37];
    for (size_t i = 0; i < sizeof(data); ++i) {
        data[i] = static_cast<u8>(i * 7);
    }
    std::memcpy(wide, data, sizeof(wide));
    u8* p = wide;
    const u32 next = g_avx2 ? Wide8(state, start, p, wide + 512) : Wide4(state, start, p, wide + 512);
    if (p != wide + 512 || next != start + 8u) {
        return false;
    }
    for (size_t i = 0; i < 512; ++i) {
        if (wide[i] != static_cast<u8>(data[i] ^ one[i])) {
            return false;
        }
    }
    // Fast() from an unaligned offset: byte by byte like the game.
    u8 object[8 + 64] = {};
    std::memcpy(object + 8, state, 64);
    const u64 offset = 64ull * 5 + 13;
    reinterpret_cast<u32*>(object + 8)[12] = static_cast<u32>(offset >> 6);
    u8 fast[sizeof(data)];
    std::memcpy(fast, data, sizeof(data));
    Fast(object, fast, sizeof(fast), offset);
    u32 counter = static_cast<u32>(offset >> 6);
    u8 key[64];
    Block(state, counter, key);
    u32 pos = static_cast<u32>(offset & 63);
    for (size_t i = 0; i < sizeof(data); ++i) {
        if (fast[i] != static_cast<u8>(data[i] ^ key[pos])) {
            return false;
        }
        if (pos == 63) {
            ++counter;
            Block(state, counter, key);
            pos = 0;
        } else {
            ++pos;
        }
    }
    return reinterpret_cast<u32*>(object + 8)[12] == counter;
}

void Install(u8* base) {
    const char* env = std::getenv("RYGT_FAST_CIPHER");
    const bool asked = env && env[0];
    if (asked && env[0] == '0') {
        return;
    }
    u8* const entry = base + kXor + kTextRvaShift;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(entry, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        std::memcmp(entry, kEntryBytes, sizeof(kEntryBytes)) != 0 ||
        std::memcmp(base + kLoop + kTextRvaShift, kLoopBytes, sizeof(kLoopBytes)) != 0 ||
        std::memcmp(base + kEpilogue + kTextRvaShift, kTailBytes, sizeof(kTailBytes)) != 0) {
        if (asked) {
            std::fprintf(stderr, "[patch] PERF-GT7-113: original bytes differ, not applied\n");
        }
        return;
    }
    int regs[4] = {};
    __cpuidex(regs, 7, 0);
    g_avx2 = (regs[1] & (1 << 5)) != 0;
    if (const char* wide = std::getenv("RYGT_FAST_CIPHER_SSE"); wide && wide[0] == '1') {
        g_avx2 = false;
    }
    if (!SelfTest()) {
        std::fprintf(stderr, "[patch] PERF-GT7-113: self-test failed, not applied\n");
        return;
    }
    u8* stub = AllocNear(base);
    if (!stub) {
        std::fprintf(stderr, "[patch] PERF-GT7-113: no memory for the stub, not applied\n");
        return;
    }
    g_original = reinterpret_cast<XorFn>(entry);
    g_default_seek = reinterpret_cast<u64>(base + kDefaultSeek + kTextRvaShift);
    const u64 default_next = reinterpret_cast<u64>(base + kDefaultNext + kTextRvaShift);
    const u64 hook = reinterpret_cast<u64>(&Hook);
    const u64 epilogue = reinterpret_cast<u64>(base + kEpilogue + kTextRvaShift);
    const u64 resume = reinterpret_cast<u64>(base + kResume + kTextRvaShift);
    std::vector<u8> code;
    const auto bytes = [&](std::initializer_list<u8> list) { code.insert(code.end(), list); };
    const auto imm64 = [&](u64 value) {
        for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>(value >> (8 * i)));
    };
    bytes({0x4c, 0x8b, 0x17});       // mov r10, [rdi]          (vtable)
    bytes({0x4d, 0x8b, 0x52, 0x10}); // mov r10, [r10 + 0x10]   (block step)
    bytes({0x49, 0xbb});             // mov r11, default step
    imm64(default_next);
    bytes({0x4d, 0x39, 0xda}); // cmp r10, r11
    bytes({0x75, 0x00});       // jne fallback (patched below)
    const size_t jne_at = code.size() - 1;
    bytes({0x57, 0x56, 0x52, 0x51}); // push rdi, rsi, rdx, rcx (rsp stays 16-aligned)
    bytes({0x49, 0xbb});             // mov r11, Hook
    imm64(hook);
    bytes({0x41, 0xff, 0xd3});       // call r11
    bytes({0x59, 0x5a, 0x5e, 0x5f}); // pop rcx, rdx, rsi, rdi
    bytes({0x84, 0xc0});             // test al, al
    bytes({0x74, 0x00});             // jz fallback (patched below)
    const size_t jz_at = code.size() - 1;
    bytes({0x49, 0xbb}); // mov r11, epilogue
    imm64(epilogue);
    bytes({0x41, 0xff, 0xe3}); // jmp r11
    const size_t fallback = code.size();
    bytes({0xc5, 0xfa, 0x6f, 0x47, 0x08}); // vmovdqu xmm0, [rdi + 0x08]  (moved from the game)
    bytes({0xc5, 0xfa, 0x6f, 0x4f, 0x18}); // vmovdqu xmm1, [rdi + 0x18]
    bytes({0xc5, 0xfa, 0x6f, 0x57, 0x28}); // vmovdqu xmm2, [rdi + 0x28]
    bytes({0x49, 0xbb});                   // mov r11, resume
    imm64(resume);
    bytes({0x41, 0xff, 0xe3}); // jmp r11
    code[jne_at] = static_cast<u8>(fallback - (jne_at + 1));
    code[jz_at] = static_cast<u8>(fallback - (jz_at + 1));
    std::memcpy(stub, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), stub, code.size());

    u8* const join = base + kJoin + kTextRvaShift;
    u8 jump[15] = {0xff, 0x25, 0, 0, 0, 0}; // jmp [rip]; address; int3
    std::memcpy(jump + 6, &stub, 8);
    jump[14] = 0xcc;
    DWORD old_protect = 0;
    if (!VirtualProtect(join, sizeof(jump), PAGE_EXECUTE_READWRITE, &old_protect)) {
        return;
    }
    std::memcpy(join, jump, sizeof(jump));
    DWORD ignored = 0;
    VirtualProtect(join, sizeof(jump), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), join, sizeof(jump));
    if (const char* verify = std::getenv("RYGT_FAST_CIPHER_VERIFY"); verify && verify[0]) {
        g_verify_left = std::strtoll(verify, nullptr, 0);
    }
    if (const char* stats = std::getenv("RYGT_DIAG_CIPHER"); stats && stats[0] == '1') {
        g_stats = true;
    }
    std::fprintf(stderr, "[patch] PERF-GT7-113 fast file cipher applied (%s)\n",
                 g_avx2 ? "AVX2, 8 blocks" : "SSE, 4 blocks");
}

} // namespace Gt7Cipher

void InstallPhotoExport(); // GTS-010, gts_photo_export.cpp
void InstallFileShareDelete(); // GTS-014, gts_file_share.cpp

// GTS-012: a retail build. pdiext::MProductInformation::GetVersionBuild (eboot 0x10c5500) looks up
// "VersionBuild" in the game's product information, and in this build it says "debug". The
// scripts compare it with "debug" to switch on developer features: the Scapes spot page showed its
// debug layer (red "WWWWWWWWWW" Meta ID / Regist ID fields and "* 0"), and Photo mode has ten such
// checks too. The lookup's key becomes its default "no-build" (lea rdi points at that string), so
// the lookup misses and returns "no-build", which is not "debug". RYGT_DEBUG_BUILD=1 keeps "debug".
void InstallRetailBuild(u8* base) {
    if (const char* env = std::getenv("RYGT_DEBUG_BUILD"); env && env[0] == '1') {
        return;
    }
    // lea rdi, [rip+0xdd6595] ("VersionBuild") -> lea rdi, [rip+0xdd65a2] ("no-build")
    Apply(base, Patch{0x10c552d, {0x48, 0x8d, 0x3d, 0x95, 0x65, 0xdd, 0x00},
                      {0x48, 0x8d, 0x3d, 0xa2, 0x65, 0xdd, 0x00}, "GTS-012 retail build (VersionBuild)"});
}

void RytyApplyGamePatches() {
    Gt7Cipher::Install(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));     // PERF-GT7-113
    InstallPhotoExport();
    InstallScopedMenuCompiles(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    InstallFileShareDelete();
    InstallRetailBuild(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    InstallExperimentalSceneViewport(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    InstallMenuSnapshotSessionGuard(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    InstallIdleKeyboardClose(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    InstallWideCamera(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    InstallTriggerTelemetry(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    InstallUi4kImages(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    InstallMirrorHalfRate(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    InstallMotionBlurOption(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    if (!WetReflectionsWanted()) {
        // jne (reflect when wet) -> two nops
        Apply(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)),
              Patch{0xac27db, {0x75, 0x77}, {0x90, 0x90}, "GTS-007 wet-road reflections off"});
    }
    InstallMiscHooks(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    InstallLewisChallenge(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    InstallScriptPatches(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
#ifndef NDEBUG
    InstallTextEntryDiag(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
#endif
    InstallFatalErrorDiag(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    if (!(std::getenv("RYGT_DECALS") && std::getenv("RYGT_DECALS")[0] == '0')) {
        SyncDecals();
    }
    auto* base = reinterpret_cast<u8*>(GetModuleHandleA(nullptr));
    char buffer[4096];
    const DWORD length = GetEnvironmentVariableA("RYGT_PATCH", buffer, sizeof(buffer));
    if (length == 0 || length >= sizeof(buffer)) {
        return;
    }
    std::string list{buffer, length};
    size_t start = 0;
    while (start < list.size()) {
        const size_t end = std::min(list.find(',', start), list.size());
        const std::string item = list.substr(start, end - start);
        const size_t a = item.find(':');
        const size_t b = item.find(':', a + 1);
        if (a != std::string::npos && b != std::string::npos) {
            Patch patch{std::strtoull(item.substr(0, a).c_str(), nullptr, 16),
                        ParseHex(item.substr(a + 1, b - a - 1)), ParseHex(item.substr(b + 1)),
                        "RYGT_PATCH " + item.substr(0, a)};
            Apply(base, patch);
        }
        start = end + 1;
    }
}

namespace Vulkan {
// FIX-110: true while a race project runs (see NoteProjectScript).
bool RytyGtsInRace() {
    return g_in_race.load(std::memory_order_relaxed);
}
bool RytyGtsMenuCaptureCanContinue() {
    return g_scoped_menu_compiles && g_capture_compiles.CanContinue();
}
bool RytyGtsMenuCaptureAvailable() {
    return g_scoped_menu_compiles;
}
bool RytyGtsMenuSnapshotsAllowed() {
    return g_snapshot_session_ready && g_snapshot_session.Idle() && !RytyGtsInRace();
}
} // namespace Vulkan
