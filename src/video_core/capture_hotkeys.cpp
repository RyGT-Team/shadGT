// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// FEAT-SGT-002: RyGT-GPU-Capture's Shift+F8 and Ctrl+F8, built into the renderer. The external
// helper found the renderer's capture flags by fixed offsets of one renderer build (October 25)
// and never worked with later builds; here they need nothing outside the game.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/types.h"
#include "video_core/capture_hotkeys.h"
#include "video_core/renderdoc.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace Vulkan {
extern std::atomic<u64> g_prepared_frames; // DIAG-137, vk_presenter.cpp
}

namespace VideoCore {
namespace {

std::filesystem::path NewCaptureFolder(const char* kind) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    const auto name = fmt::format("{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}-{}", local.tm_year + 1900,
                                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                                  local.tm_sec, ms, kind);
    auto folder = std::filesystem::current_path() / "captures" / name;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    return folder;
}

void Acknowledge(unsigned frequency, unsigned ms) {
#ifdef _WIN32
    Beep(frequency, ms); // as the helper did: the game is in front, a sound says it was taken
#endif
}

struct Recording {
    std::mutex mutex;
    std::jthread thread;
    std::filesystem::path folder;
};
Recording& GetRecording() {
    static Recording recording;
    return recording;
}

#ifdef _WIN32
struct ProcessSample {
    double cpu_seconds{};
    u64 private_bytes{};
    u64 working_set{};
};
ProcessSample SampleProcess() {
    ProcessSample sample;
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto ticks = [](const FILETIME& t) {
            return (u64(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        };
        sample.cpu_seconds = double(ticks(kernel) + ticks(user)) / 1e7;
    }
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        sample.private_bytes = memory.PrivateUsage;
        sample.working_set = memory.WorkingSetSize;
    }
    return sample;
}

// nvidia-smi beside the recording, once a second, into gpu.csv (the helper's columns).
HANDLE StartNvidiaSmi(const std::filesystem::path& folder) {
    wchar_t path[MAX_PATH]{};
    if (!SearchPathW(nullptr, L"nvidia-smi.exe", nullptr, MAX_PATH, path, nullptr)) {
        return nullptr;
    }
    SECURITY_ATTRIBUTES inherit{.nLength = sizeof(SECURITY_ATTRIBUTES), .bInheritHandle = TRUE};
    HANDLE out = CreateFileW((folder / "gpu.csv").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return nullptr;
    std::wstring command = std::wstring{L"\""} + path +
                           L"\" --query-gpu=timestamp,utilization.gpu,utilization.memory,"
                           L"memory.used,clocks.current.graphics,power.draw,temperature.gpu "
                           L"--format=csv -lms 1000";
    STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = out;
    startup.hStdError = out;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(out);
    if (!started) return nullptr;
    CloseHandle(process.hThread);
    return process.hProcess;
}
#endif

void RecordPerformance(std::stop_token stop, std::filesystem::path folder) {
    std::FILE* output = nullptr;
#ifdef _WIN32
    _wfopen_s(&output, (folder / "performance.jsonl").c_str(), L"w");
    HANDLE gpu = StartNvidiaSmi(folder);
#else
    output = std::fopen((folder / "performance.jsonl").string().c_str(), "w");
#endif
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    double first_cpu = -1, last_cpu = 0, last_seconds = 0;
    u64 first_frames = Vulkan::g_prepared_frames.load(std::memory_order_relaxed), last_frames =
        first_frames;
    u64 peak_private = 0, peak_working = 0, samples = 0;
    const char* reason = "hotkey";
    while (!stop.stop_requested()) {
        const double seconds = std::chrono::duration<double>(clock::now() - start).count();
        const u64 frames = Vulkan::g_prepared_frames.load(std::memory_order_relaxed);
#ifdef _WIN32
        const auto process = SampleProcess();
#else
        const ProcessSample process{};
#endif
        if (first_cpu < 0) first_cpu = process.cpu_seconds;
        const double interval = seconds - last_seconds;
        const double threads = samples && interval > 0 ? (process.cpu_seconds - last_cpu) / interval : 0;
        const double fps = samples && interval > 0 ? double(frames - last_frames) / interval : 0;
        if (output) {
            std::fputs(fmt::format("{{\"elapsed_seconds\":{:.3f},\"process_cpu_seconds\":{:.3f},"
                                   "\"cpu_threads_used\":{:.2f},\"frames\":{},\"fps\":{:.1f},"
                                   "\"private_bytes\":{},\"working_set_bytes\":{}}}\n",
                                   seconds, process.cpu_seconds, threads, frames - first_frames,
                                   fps, process.private_bytes, process.working_set)
                           .c_str(),
                       output);
            std::fflush(output);
        }
        peak_private = std::max(peak_private, process.private_bytes);
        peak_working = std::max(peak_working, process.working_set);
        last_cpu = process.cpu_seconds;
        last_seconds = seconds;
        last_frames = frames;
        ++samples;
        if (seconds >= 1800) {
            reason = "30-minute limit";
            break;
        }
        for (int i = 0; i < 25 && !stop.stop_requested(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }
#ifdef _WIN32
    if (gpu) {
        TerminateProcess(gpu, 0);
        CloseHandle(gpu);
    }
#endif
    if (output) std::fclose(output);
    const u64 frames = last_frames - first_frames;
    if (std::FILE* summary = std::fopen((folder / "performance.json").string().c_str(), "w")) {
        std::fputs(fmt::format("{{\n  \"continuous\": true,\n  \"samples\": {},\n  \"stop_reason\": "
                               "\"{}\",\n  \"duration_seconds\": {:.3f},\n  \"frames\": {},\n  "
                               "\"average_fps\": {:.1f},\n  \"cpu_seconds\": {:.3f},\n  "
                               "\"average_cpu_threads\": {:.2f},\n  \"peak_sampled_private_bytes\": "
                               "{},\n  \"peak_sampled_working_set_bytes\": {},\n  \"note\": \"250 ms "
                               "process samples; 1 s GPU samples in gpu.csv when nvidia-smi is "
                               "installed (FEAT-SGT-002)\"\n}}\n",
                               samples, reason, last_seconds, frames,
                               last_seconds > 0 ? double(frames) / last_seconds : 0.0,
                               first_cpu < 0 ? 0.0 : last_cpu - first_cpu,
                               last_seconds > 0 && first_cpu >= 0 ? (last_cpu - first_cpu) / last_seconds
                                                                  : 0.0,
                               peak_private, peak_working)
                       .c_str(),
                   summary);
        std::fclose(summary);
    }
    LOG_WARNING(Render_Vulkan, "FEAT-SGT-002: performance recording saved ({}): {}", reason,
                folder.string());
    Acknowledge(800, 100);
}

} // namespace

void CaptureBurst() {
    // As the helper: up to 24 consecutive frames. The presenter takes one requested path a frame.
    constexpr u32 frames = 24;
    const auto folder = NewCaptureFolder("burst") / "visible-burst";
    for (u32 i = 0; i < frames; ++i) {
        RequestScreenshotToPath(folder / fmt::format("{:03}.png", i));
    }
    // The user's ask: a frame dump of the same frames too, so they can be compared (its own folder,
    // logs\dump-f8-...; builds with diagnostics only, as F8's).
#ifndef NDEBUG
    const bool dumped = RequestDebugFrameDump(frames);
#else
    const bool dumped = false;
#endif
    LOG_WARNING(Render_Vulkan, "FEAT-SGT-002: Shift+F8: {} consecutive frames to {}{}", frames,
                folder.string(), dumped ? ", and a frame dump of them" : "");
    Acknowledge(1100, 60);
}

void TogglePerformanceRecording() {
    auto& recording = GetRecording();
    std::scoped_lock lock{recording.mutex};
    if (recording.thread.joinable()) {
        recording.thread.request_stop();
        recording.thread.join(); // the sampler stops within 10 ms
        RequestScreenshotToPath(recording.folder / "visible-stop.png");
        return;
    }
    recording.folder = NewCaptureFolder("performance");
    RequestScreenshotToPath(recording.folder / "visible.png");
    recording.thread = std::jthread(RecordPerformance, recording.folder);
    LOG_WARNING(Render_Vulkan, "FEAT-SGT-002: Ctrl+F8: performance recording started: {}",
                recording.folder.string());
    Acknowledge(1300, 60);
}

void StopPerformanceRecording() {
    auto& recording = GetRecording();
    std::scoped_lock lock{recording.mutex};
    if (recording.thread.joinable()) {
        recording.thread.request_stop();
        recording.thread.join();
    }
}

} // namespace VideoCore
