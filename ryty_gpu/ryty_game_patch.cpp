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
#include <bit>
#include <array>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>
#include <windows.h>
#include <fmt/format.h>

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
struct RytySceneExtent {
    std::uint32_t width{}, height{};
};
RytySceneExtent RytySceneSize(); // WIDE-001
bool RytyWideScene();
bool RytyWideViews();
bool RytyGtsInRace();
float RytyDisplayAspect();
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
// WIDE-015: the race "session" for the wide picture: from a race_* project until the menus proper.
// The pre-race screen's own pages (Change Conditions, Change Car, Select Course) load menu projects
// (course_select, car_select, garage, ...) on top of the race and return to it without reloading
// it, so those keep the session (and the race's wide cameras) going.
std::atomic<bool> g_race_session{false};
// WIDE-059: the replay's photo mode is open (photo, with gallery loaded along).
std::atomic<bool> g_photo_mode{false};
std::atomic<int> g_menu_screen{0}; // WIDE-020: 1 main menu, 2 Home, 3 livery editor

// WIDE-013: the cameras the race's own screens create (the pre-race screen's overhead view) take
// their aspect from two constructor defaults (fov 60, near 0.1, far 5000, aspect 16:9 at
// 0x141dee040 (0x6bea53) and 0x141dee0c0 (0x714ea3)). The game's menus use them too and are
// composed 16:9, so they are widened only while a race project is loaded.
u8* g_wide_base = nullptr;
float g_wide_aspect = 0.0f;

void SetRaceViewDefaults(bool race) {
    if (!g_wide_base || g_wide_aspect <= 0.0f) {
        return;
    }
    const float value = race ? g_wide_aspect : 16.0f / 9.0f;
    for (const u64 rva : {0x1dee04cull, 0x1dee0ccull}) {
        u8* target = g_wide_base + rva;
        DWORD old_protect = 0;
        if (!VirtualProtect(target, 4, PAGE_READWRITE, &old_protect)) {
            continue;
        }
        std::memcpy(target, &value, 4);
        DWORD ignored = 0;
        VirtualProtect(target, 4, old_protect, &ignored);
    }
    std::fprintf(stderr, "[patch] WIDE-013 race view defaults: aspect %.3f\n", value);
}


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
    // WIDE-058: the replay's photo mode (photo, with gallery loaded along) is part of the session
    // too: as a menu it set the views back to 16:9 and the replay stayed 16:9 in the middle
    // with its TV cameras until the replay was loaded again.
    static constexpr std::string_view race_pages[] = {
        "course_select", "car_select", "garage", "event_setting", "preview", "car_preview",
        "option", "photo", "gallery"};
    const bool session = race || (g_race_session.load(std::memory_order_relaxed) &&
                                  std::find(std::begin(race_pages), std::end(race_pages), name) !=
                                      std::end(race_pages));
    g_photo_mode.store(name == "photo" || (name == "gallery" && g_photo_mode.load()),
                       std::memory_order_relaxed); // WIDE-059
    if (g_race_session.exchange(session) != session) {
        std::fprintf(stderr, "[patch] WIDE-015 %.*s: %s\n", static_cast<int>(name.size()),
                     name.data(), session ? "race screens (wide)" : "menus");
    }
    // WIDE-020: the menu screens shown wide (their 3D scenes full width): the main menu (gtmode),
    // Home (garage) and the livery editor (livery2, which loads gallery along with it).
    int screen = 0;
    if (!session) {
        if (name == "gtmode") screen = 1;
        else if (name == "garage") screen = 2;
        else if (name == "livery2") screen = 3;
        else if (name == "gallery" && g_menu_screen.load(std::memory_order_relaxed) == 3) screen = 3;
    }
    // WIDE-041: the menu screens show the game's 16:9 picture in the middle with ambient sides,
    // so their views keep the game's 16:9 aspect.
    if (g_menu_screen.exchange(screen) != screen) {
        std::fprintf(stderr, "[patch] WIDE-020 %.*s: menu screen %d\n", static_cast<int>(name.size()),
                     name.data(), screen);
    }
    const bool wide_views = session; // WIDE-041: not on the menu screens
    static std::atomic<int> views_were{-1};
    if (views_were.exchange(wide_views ? 1 : 0) != (wide_views ? 1 : 0)) {
        SetRaceViewDefaults(wide_views);
    }
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

// WIDE-001: a genuine wide picture. With a camera wider than 16:9 (GTS-009), the high-resolution
// UI (ui_hires) and the native renderer, the scene is allocated at the screen's shape instead of
// 16:9, the race picture is composed at the window's full size, and the HUD and menus are drawn
// 16:9 in its middle (nothing is stretched). Opt-in: GPU.wide_scene=true or RYGT_WIDE_SCENE=1
// turns it on; off, a wider screen keeps GTS-009's 16:9 picture.
bool RytyWideScene() {
    static const bool wide = [] {
        if (RytyDisplayAspect() <= 16.0f / 9.0f + 0.01f) {
            return false; // only wider screens
        }
        if (const char* env = std::getenv("RYGT_WIDE_SCENE")) {
            return env[0] == '1';
        }
        return ConfigFlag("wide_scene", false) && ConfigFlag("ui_hires", false) &&
               ConfigFlag("native_renderer", false);
    }();
    return wide;
}

// WIDE-002/004: the game's 3D views widened to the screen's shape (their 16:9 constants patched).
// GPU.wide_views=false or RYGT_WIDE_VIEWS=0 leaves them at 16:9.
bool RytyWideViews() {
    static const bool views = [] {
        if (!RytyWideScene()) return false;
        if (const char* env = std::getenv("RYGT_WIDE_VIEWS")) return env[0] == '1';
        return ConfigFlag("wide_views", true);
    }();
    return views;
}

// WIDE-005: the race HUD's left and right parts moved out to the screen's edges.
// GPU.wide_hud_spread=false or RYGT_WIDE_HUD_SPREAD=0 keeps the HUD 16:9 in the middle.
bool RytyHudSpread() {
    static const bool spread = [] {
        if (!RytyWideScene()) return false;
        if (const char* env = std::getenv("RYGT_WIDE_HUD_SPREAD")) return env[0] == '1';
        return ConfigFlag("wide_hud_spread", true);
    }();
    return spread;
}

// The scene's size when it is not the game's own 1920x1080: the launcher's scene render scale
// (RYGT_SCENE_WIDTH/HEIGHT, always 16:9), widened to the screen's shape with WIDE-001.
// {0, 0} when the game's own size applies.
RytySceneExtent RytySceneSize() {
    static const RytySceneExtent size = [] {
        RytySceneExtent result{};
        const char* viewport = std::getenv("RYGT_EXPERIMENTAL_SCENE_VIEWPORT");
        if (viewport && std::strcmp(viewport, "1") == 0) {
            const char* w = std::getenv("RYGT_SCENE_WIDTH");
            const char* h = std::getenv("RYGT_SCENE_HEIGHT");
            result.width = w ? u32(std::strtoul(w, nullptr, 10)) : 960u;
            result.height = h ? u32(std::strtoul(h, nullptr, 10)) : 540u;
        }
        // GPU.wide_scene_alloc=false keeps the scene's allocation 16:9 (the wide picture is then
        // composed from it at a lower horizontal resolution); for tests.
        if (RytyWideScene() && ConfigFlag("wide_scene_alloc", true)) {
            if (!result.width || !result.height) {
                result = {1920, 1080};
            }
            // GPU.wide_scene_height (e.g. 1440 for 5120x1440 on a 32:9 screen) overrides the height.
            if (const u32 wanted = u32(std::atoi(ConfigValue("wide_scene_height").c_str()));
                wanted >= 360 && wanted <= 2160) {
                result.height = wanted;
            }
            const double aspect = RytyDisplayAspect();
            constexpr double max_pixels = 3840.0 * 2160.0; // what the scene scale allows at 16:9
            u32 height = result.height;
            if (double(height) * height * aspect > max_pixels) {
                height = u32(std::sqrt(max_pixels / aspect)) & ~7u;
            }
            const u32 width = std::min<u32>(u32(std::lround(height * aspect / 16.0)) * 16, 7680);
            std::fprintf(stderr, "[patch] WIDE-001 scene %ux%u (was %ux%u), camera aspect %.3f\n",
                         width, height, result.width, result.height, aspect);
            result = {width, height};
        }
        if (result.width < 320 || result.width > 7680 || result.height < 180 ||
            result.height > 2160 || (result.width == 1920 && result.height == 1080)) {
            return RytySceneExtent{};
        }
        return result;
    }();
    return size;
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
        std::fprintf(stderr, Vulkan::RytyWideScene()
                                 ? "[patch] GTS-009: wide picture (WIDE-001), HUD and menus 16:9\n"
                                 : "[patch] GTS-009: picture stretched to the window\n");
    }
    if (!Vulkan::RytyWideScene()) {
        return;
    }
    // WIDE-002: the 16:9 that the patch above does not reach.
    // - The camera code sets the active view's aspect to 16:9 every frame (GTSport.exe 0x85fb6b:
    //   movss xmm0, [0x141ddf260]; movss [r13+0x2c], xmm0), which it then copies into the global
    //   the camera's FOV code reads (0x8708d6 -> [0x142b84100]).
    // - Render views are constructed with a 16:9 projection aspect (aspect, 0, 0, near 0.1 at
    //   view+0x5c, from the vectors at 0x141db2890, 0x141def5a0, 0x141def5c0, 0x141def5e0;
    //   constructors around 0x8cf170), which the FOV code divides by (0x892e83).
    // GPU.wide_views=false or RYGT_WIDE_VIEWS=0 leaves them at 16:9.
    if (!Vulkan::RytyWideViews()) {
        return;
    }
    for (const u64 rva : {0x1ddf260ull, 0x1db2890ull, 0x1def5a0ull, 0x1def5c0ull, 0x1def5e0ull}) {
        Apply(base, Patch{rva, from, to, fmt::format("WIDE-002 view aspect {:.3f}", aspect)});
    }
    // WIDE-004: the race renderer's own 16:9 (the camera's aspect, view[23], is ignored there):
    // 0x141dddd14 (0x641ab4, 0x641ef4), 0x141dddd40 (0x6455c5, 0x6457f5), 0x141dddd4c (0x647336,
    // 0x647574), 0x141dddeac (0x6a231d) and the frame setup's 0x141decd78 (0x199c4c4:
    // width = tan(fov / 2) * 16 / 9). Each constant is read only there.
    for (const u64 rva : {0x1dddd14ull, 0x1dddd40ull, 0x1dddd4cull, 0x1dddeacull, 0x1decd78ull}) {
        Apply(base, Patch{rva, from, to, fmt::format("WIDE-004 render aspect {:.3f}", aspect)});
    }
    // WIDE-013: checked here, written when a race project loads (SetRaceViewDefaults).
    if (std::memcmp(base + 0x1dee04c, from.data(), 4) == 0 &&
        std::memcmp(base + 0x1dee0cc, from.data(), 4) == 0) {
        g_wide_base = base;
        g_wide_aspect = aspect;
    }
    // Views with an aspect set by the game's scripts (flag [r12+0x4f0], value [r12+0x4f4], set to
    // 16:9 by the setter at 0x8593f0) take it instead (0x86015f: vmovss xmm0, [r12+0x4f4]). That
    // load reads the patched constant at 0x141ddf260 instead (vmovss xmm0, [rip+...]; nop).
    Apply(base, Patch{0x86015f, {0xc4, 0xc1, 0x7a, 0x10, 0x84, 0x24, 0xf4, 0x04, 0x00, 0x00},
                      {0xc5, 0xfa, 0x10, 0x05, 0xf9, 0xf0, 0x57, 0x01, 0x66, 0x90},
                      "WIDE-002 scripted view aspect"});
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
    static const u32 scene_width = Vulkan::RytySceneSize().width;   // WIDE-001
    static const u32 scene_height = Vulkan::RytySceneSize().height;
    u32 output_extent{}, scene_extent{};
    // These are verified color-target hint fields, not guessed ImageInfo offsets.
    if (output && scene && scene_x == 1.0f && scene_y == 1.0f &&
        Core::RytyMemory::GuardedCopy(&output_extent,
            reinterpret_cast<const void*>(output + 0x44), sizeof(output_extent)) &&
        Core::RytyMemory::GuardedCopy(&scene_extent,
            reinterpret_cast<const void*>(scene + 0x44), sizeof(scene_extent)) &&
        output_extent == 0x04380780 && scene_width >= 320 && scene_width <= 7680 &&
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
    const u32 width = Vulkan::RytySceneSize().width; // WIDE-001
    const u32 height = Vulkan::RytySceneSize().height;
    if (width < 320 || width > 7680 || height < 180 || height > 2160 ||
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
    if ((!enabled || std::strcmp(enabled, "1") != 0) && !Vulkan::RytyWideScene()) return;
    if (Vulkan::RytySceneSize().width == 0) return;
    InstallSceneHistoryOrigins(base);
    Native::NearCode code{base};
    constexpr std::array<u8, 6> original{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    g_scene_viewport = reinterpret_cast<SceneViewportFn>(Native::HookEntry(
        code, base, 0x18d2cb0, original,
        reinterpret_cast<void*>(&MatchExperimentalSceneViewport), "experimental scene viewport scale"));
}

// WIDE-001: the scene allocated at the screen's shape. The same three sites the launcher's scene
// render scale patches (RenderResolution.cs), from the game's bytes or from the launcher's.
static void InstallWideSceneAllocation(u8* base) {
    if (!Vulkan::RytyWideScene() || !ConfigFlag("wide_scene_alloc", true)) {
        return;
    }
    const auto [width, height] = Vulkan::RytySceneSize();
    if (!width || !height) {
        return;
    }
    const auto le = [](u32 value) {
        std::array<u8, 4> bytes{};
        std::memcpy(bytes.data(), &value, 4);
        return bytes;
    };
    const auto current = [&](u64 rva, size_t size) {
        return std::vector<u8>(base + rva, base + rva + size);
    };
    // mov rcx, (height << 32) | width
    auto size_from = current(0x1988ad3, 10);
    if (size_from[0] != 0x48 || size_from[1] != 0xb9) {
        std::fprintf(stderr, "[patch] WIDE-001: scene size site differs; not applied\n");
        return;
    }
    std::vector<u8> size_to{0x48, 0xb9};
    for (const u8 b : le(width)) size_to.push_back(b);
    for (const u8 b : le(height)) size_to.push_back(b);
    // mov ecx, [rip+...] (the game) or mov ecx, imm32; nop (the launcher) -> mov ecx, imm32; nop
    const auto one = [&](u64 rva, std::array<u8, 6> original, u32 value) {
        auto from = current(rva, 6);
        const bool game = std::equal(from.begin(), from.end(), original.begin());
        const bool launcher = from[0] == 0xb9 && from[5] == 0x90;
        if (!game && !launcher) {
            return std::optional<Patch>{};
        }
        std::vector<u8> to{0xb9};
        for (const u8 b : le(value)) to.push_back(b);
        to.push_back(0x90);
        return std::optional<Patch>{Patch{rva, from, to, fmt::format("WIDE-001 scene {}", value)}};
    };
    const auto w = one(0x1988ab0, {0x8b, 0x0d, 0x72, 0xea, 0x23, 0x01}, width);
    const auto h = one(0x1988ab9, {0x8b, 0x0d, 0x6d, 0xea, 0x23, 0x01}, height);
    if (!w || !h) {
        std::fprintf(stderr, "[patch] WIDE-001: scene width/height sites differ; not applied\n");
        return;
    }
    if (size_from != size_to) {
        Apply(base, Patch{0x1988ad3, size_from, size_to,
                          fmt::format("WIDE-001 scene allocation {}x{}", width, height)});
    }
    if (w->original != w->replacement) Apply(base, *w);
    if (h->original != h->replacement) Apply(base, *h);
}

static void ApplyEnvironmentPatches(u8* base) {
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
    ApplyEnvironmentPatches(base);
    InstallWideSceneAllocation(base); // WIDE-001: after the launcher's scene size
}

namespace Vulkan {
// FIX-110: true while a race project runs (see NoteProjectScript).
bool RytyGtsInRace() {
    return g_in_race.load(std::memory_order_relaxed);
}
// WIDE-015: true from a race project until the menus proper (its own pages included).
bool RytyGtsRaceSession() {
    return g_race_session.load(std::memory_order_relaxed);
}
// WIDE-059: the replay's photo mode is open.
bool RytyGtsPhotoMode() {
    return g_photo_mode.load(std::memory_order_relaxed);
}
// WIDE-020: the menu screen shown wide (1 main menu, 2 Home, 3 livery editor), or 0.
int RytyGtsMenuScreen() {
    return g_menu_screen.load(std::memory_order_relaxed);
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
