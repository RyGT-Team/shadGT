// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/shader_capture.h"

#if defined(RYTY_GPU) && defined(_WIN32)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/read_capture.h"
#include "common/thread.h"
#include "video_core/amdgpu/regs_shader.h"

namespace Vulkan {
bool RytyRaceRunning(); // GAME-006, ryty_game_patch.cpp
} // namespace Vulkan

namespace Vulkan::ShaderCapture {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t Chunk = size_t{4} << 20;
constexpr size_t Footer = sizeof(AmdGpu::BinaryInfo);
constexpr u64 MaxProgram = u64{4} << 20; // larger lengths in a footer are not code
constexpr u32 Tries = 3;                 // scans that may fail to copy a program before it is left out

struct Pending {
    u32 bytes; // the largest code its records name
    u32 failures;
};

std::filesystem::path g_programs_dir; // set before the thread starts
std::filesystem::path g_manifest_dir;
std::filesystem::path g_capture_dir;

// Scans asked by ScanNowAndWait.
std::mutex g_request_mutex;
std::condition_variable g_request_cv;
u64 g_requested = 0; // under g_request_mutex
u64 g_served = 0;
bool g_last_scanned = false;
std::atomic<double> g_last_scan_seconds{0.0};
std::atomic<bool> g_running{false};
std::atomic<u64> g_unserved{0};

bool ReadableView(const MEMORY_BASIC_INFORMATION& mbi) {
    const DWORD prot = mbi.Protect & 0xff;
    return mbi.State == MEM_COMMIT && mbi.Type == MEM_MAPPED && !(mbi.Protect & PAGE_GUARD) &&
           !(mbi.Protect & (PAGE_NOCACHE | PAGE_WRITECOMBINE)) && // video memory views: never read
           (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READ ||
            prot == PAGE_EXECUTE_READWRITE || prot == PAGE_WRITECOPY ||
            prot == PAGE_EXECUTE_WRITECOPY);
}

// The manifest's programs whose code this PC lacks, with the size their records name.
std::unordered_map<u64, Pending> ReadPending(u64& bound) {
    std::unordered_map<u64, Pending> pending;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(g_manifest_dir, ec)) {
        const auto name = entry.path().filename().string();
        if (name.size() != 16 + 1 + 16 + 4 || !name.ends_with(".rec") || name[16] != '-') continue;
        // The header every record format starts with: magic, version, stages, hash, code hash, words.
        u8 head[40];
        std::FILE* f = std::fopen(entry.path().string().c_str(), "rb");
        if (!f) continue;
        const bool read = std::fread(head, 1, sizeof(head), f) == sizeof(head);
        std::fclose(f);
        if (!read || std::memcmp(head, "RYGTMAN1", 8) != 0) continue;
        u64 hash;
        u32 words;
        std::memcpy(&hash, head + 20, 8);
        std::memcpy(&words, head + 36, 4);
        if (words == 0 || u64{words} * 4 > MaxProgram) continue;
        auto& one = pending[hash];
        one.bytes = std::max(one.bytes, words * 4);
    }
    // Code already here: captured before (programs folder) or written when this PC translated it.
    for (const auto* dir : {&g_programs_dir, &g_capture_dir}) {
        for (const auto& entry : std::filesystem::directory_iterator(*dir, ec)) {
            const auto name = entry.path().filename().string();
            if (name.size() >= 16 && (name.ends_with(".bin") || name.ends_with(".code"))) {
                pending.erase(std::strtoull(name.substr(0, 16).c_str(), nullptr, 16));
            }
        }
    }
    bound = 16u << 20; // a margin for footers that name a little more than the records
    for (const auto& [hash, one] : pending) bound += one.bytes;
    return pending;
}

// Copies one program's code to <hash>.bin (written aside, then renamed: a cut file is never left).
// False when its start isn't found or memory can't be read; `write_failed` on a file error.
bool Capture(u64 hash, u32 length, u64 footer, bool& write_failed) {
    const u64 back = std::min<u64>(u64{length} + 0x10000, MaxProgram);
    const u64 low = (footer > back ? footer - back : 0) & ~u64{0xff};
    const u64 first = footer & ~u64{0xff};
    if (first <= low || length < 8 || length % 4 != 0) return false;
    std::vector<u32> block(size_t(first - low) / 4 + 2);
    if (!Core::RytyMemory::GuardedCopy(block.data(), reinterpret_cast<const void*>(low),
                                       block.size() * 4)) {
        return false;
    }
    const auto start = FindProgramStart(block, low, footer);
    if (!start) return false;
    std::vector<u32> code(length / 4);
    if (!Core::RytyMemory::GuardedCopy(code.data(), reinterpret_cast<const void*>(*start), length)) {
        return false;
    }
    const auto file = g_programs_dir / fmt::format("{:016x}.bin", hash);
    auto aside = file;
    aside += ".part";
    std::FILE* f = std::fopen(aside.string().c_str(), "wb");
    if (!f) {
        write_failed = true;
        return false;
    }
    const bool ok = std::fwrite(code.data(), 4, code.size(), f) == code.size();
    const bool closed = std::fclose(f) == 0;
    std::error_code ec;
    if (ok && closed) std::filesystem::rename(aside, file, ec);
    if (!ok || !closed || ec) {
        std::filesystem::remove(aside, ec);
        write_failed = true;
        return false;
    }
    return true;
}

struct Totals {
    u64 captured = 0, failed = 0, left_out = 0, bytes = 0, bound = 0;
    bool stopped = false; // a write failed or the bound was reached
};

// One pass over the guest arena's mapped, committed, readable views, where every program was found
// (DIAG-SGT-074). Each 4 MB chunk is checked again just before it is read, and read through Ryty's
// guarded copy: a range the game unmaps or protects meanwhile is skipped.
void Scan(std::unordered_map<u64, Pending>& pending, Totals& totals, std::vector<u8>& buffer) {
    const auto& sig = AmdGpu::BinaryInfo::signature_ref;
    const auto [arena_low, arena_high] = Core::RytyMemory::GuestArena();
    u8* address = reinterpret_cast<u8*>(arena_low);
    MEMORY_BASIC_INFORMATION mbi{};
    while (!pending.empty() && !totals.stopped &&
           reinterpret_cast<u64>(address) < arena_high &&
           VirtualQuery(address, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        u8* const base = static_cast<u8*>(mbi.BaseAddress);
        const size_t size = mbi.RegionSize;
        if (ReadableView(mbi) && size >= Footer) {
            for (size_t offset = 0; offset < size && !pending.empty(); offset += Chunk) {
                if (RytyRaceRunning()) return; // never during a race, not even to finish a pass
                MEMORY_BASIC_INFORMATION now{};
                if (VirtualQuery(base + offset, &now, sizeof(now)) != sizeof(now) || !ReadableView(now)) {
                    continue;
                }
                size_t length = std::min(Chunk + Footer, size - offset); // a footer read whole
                length = std::min(length, size_t(static_cast<u8*>(now.BaseAddress) + now.RegionSize -
                                                  (base + offset)));
                if (length < Footer || !Core::RytyMemory::GuardedCopy(buffer.data(), base + offset, length)) {
                    continue;
                }
                const u8* p = buffer.data();
                const u8* const last = buffer.data() + std::min(Chunk, length - Footer + 1);
                while (p < last) {
                    p = static_cast<const u8*>(std::memchr(p, sig[0], size_t(last - p)));
                    if (!p) break;
                    if (std::memcmp(p, sig.data(), sig.size()) == 0) {
                        AmdGpu::BinaryInfo info;
                        std::memcpy(&info, p, sizeof(info));
                        const auto it = info.Valid() ? pending.find(info.shader_hash) : pending.end();
                        if (it != pending.end() && info.length != 0 && info.length <= MaxProgram) {
                            if (totals.bytes + info.length > totals.bound) {
                                totals.stopped = true;
                                LOG_WARNING(Render_Vulkan, "FEAT-SGT-117: the capture stops at {} MB, "
                                            "what the records name", totals.bound >> 20);
                                return;
                            }
                            bool write_failed = false;
                            const u64 footer = reinterpret_cast<u64>(base + offset) + u64(p - buffer.data());
                            if (Capture(it->first, u32(info.length), footer, write_failed)) {
                                ++totals.captured;
                                totals.bytes += info.length;
                                pending.erase(it);
                            } else if (write_failed) {
                                totals.stopped = true;
                                LOG_WARNING(Render_Vulkan, "FEAT-SGT-117: the capture stops: {} can't be "
                                            "written", g_programs_dir.string());
                                return;
                            } else if (++it->second.failures >= Tries) {
                                ++totals.left_out;
                                pending.erase(it);
                            } else {
                                ++totals.failed;
                            }
                        }
                    }
                    ++p;
                }
            }
        }
        u8* const next = base + size;
        if (reinterpret_cast<uintptr_t>(next) <= reinterpret_cast<uintptr_t>(base)) break;
        address = next;
    }
}

void Loop() {
    Common::SetCurrentThreadName("shadGT:ShaderCapture");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    Totals totals;
    auto pending = ReadPending(totals.bound);
    const size_t wanted = pending.size();
    LOG_WARNING(Render_Vulkan, "FEAT-SGT-117: preparation: {} recorded programs without code here, up "
                "to {} MB, captured into {}", wanted, totals.bound >> 20, g_programs_dir.string());
    std::error_code ec;
    std::filesystem::create_directories(g_programs_dir, ec);
    std::vector<u8> buffer(Chunk + Footer);
    // Between scans, at least 2 s and three times the last scan: a quarter of a core at most.
    constexpr auto Period = std::chrono::seconds{2};
    auto next_scan = Clock::now();
    auto next_report = Clock::now() + std::chrono::seconds{30};
    u64 reported = ~u64{0};
    for (;;) {
        u64 asked = 0;
        {
            std::unique_lock lk{g_request_mutex};
            g_request_cv.wait_until(lk, std::min(next_scan, next_report),
                                    [] { return g_requested > g_served; });
            if (g_requested > g_served) asked = g_requested;
        }
        bool scanned = false;
        if ((asked != 0 || Clock::now() >= next_scan) && !RytyRaceRunning() && !pending.empty() &&
            !totals.stopped) {
            const auto started = Clock::now();
            Scan(pending, totals, buffer);
            const auto took = Clock::now() - started;
            g_last_scan_seconds.store(std::chrono::duration<double>(took).count(), std::memory_order_relaxed);
            next_scan = Clock::now() + std::max<Clock::duration>(Period, 3 * took);
            scanned = true;
        } else if (Clock::now() >= next_scan) {
            next_scan = Clock::now() + Period;
        }
        if (asked != 0) {
            {
                std::scoped_lock lk{g_request_mutex};
                g_served = asked;
                g_last_scanned = scanned;
            }
            g_request_cv.notify_all();
        }
        if (Clock::now() >= next_report || (pending.empty() && reported != totals.captured)) {
            next_report = Clock::now() + std::chrono::seconds{30};
            if (reported != totals.captured || pending.empty()) {
                reported = totals.captured;
                LOG_WARNING(Render_Vulkan, "FEAT-SGT-117: {} of {} programs captured ({} MB), {} left out "
                            "after {} tries, {} copies to try again, {} scans asked and not served in time",
                            totals.captured, wanted, totals.bytes >> 20, totals.left_out, Tries,
                            totals.failed, g_unserved.load(std::memory_order_relaxed));
            }
        }
    }
}

} // namespace

bool Enabled() {
    static const bool on = [] {
        const char* env = std::getenv("SHADGT_PREPARE_SHADERS");
        return env && env[0] == '1';
    }();
    return on;
}

void Start(const std::filesystem::path& programs_dir, const std::filesystem::path& manifest_dir,
           const std::filesystem::path& capture_dir) {
    if (!Enabled()) return;
    static std::once_flag once;
    std::call_once(once, [&] {
        g_programs_dir = programs_dir;
        g_manifest_dir = manifest_dir;
        g_capture_dir = capture_dir;
        g_running.store(true, std::memory_order_release);
        std::thread(Loop).detach();
    });
}

bool ScanNowAndWait() {
    if (!g_running.load(std::memory_order_acquire)) return false;
    // As long as the last scans took, with a margin: a slow PC scans slower, and waits longer.
    const double limit = std::max(5.0, 4.0 * g_last_scan_seconds.load(std::memory_order_relaxed));
    std::unique_lock lk{g_request_mutex};
    const u64 ticket = ++g_requested;
    g_request_cv.notify_all();
    if (!g_request_cv.wait_for(lk, std::chrono::duration<double>(limit),
                               [ticket] { return g_served >= ticket; })) {
        g_unserved.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return g_last_scanned;
}

namespace {
std::atomic<bool> g_hold_translations{false}; // FEAT-SGT-127
} // namespace

void HoldTranslations(bool on) {
    g_hold_translations.store(on && Enabled(), std::memory_order_relaxed);
}

bool HoldsTranslations() {
    return g_hold_translations.load(std::memory_order_relaxed) && !RytyRaceRunning();
}

} // namespace Vulkan::ShaderCapture

#else

#include <cstdlib>

#include "common/logging/log.h"

namespace Vulkan::ShaderCapture {
bool Enabled() {
    return false;
}
void Start(const std::filesystem::path&, const std::filesystem::path&, const std::filesystem::path&) {
    const char* env = std::getenv("SHADGT_PREPARE_SHADERS");
    if (env && env[0] == '1') {
        LOG_WARNING(Render_Vulkan, "FEAT-SGT-117: the preparation's capture runs only under Ryty on "
                                   "Windows; nothing is captured");
    }
}
bool ScanNowAndWait() {
    return false;
}
void HoldTranslations(bool) {}
bool HoldsTranslations() {
    return false;
}
} // namespace Vulkan::ShaderCapture

#endif
