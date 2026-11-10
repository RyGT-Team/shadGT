// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <cstddef>

#ifdef _WIN32
#include <windows.h>
#endif

#include "video_core/renderer_vulkan/snapshot_source_lease.h"

namespace Vulkan::SnapshotLease {
using UnsafeCallback = void (*)(std::uint64_t, std::uint64_t);
using ProtectionBeginCallback = int (*)();
using ProtectionEndCallback = void (*)();
using ClampedProtectionCallback = int (*)(std::uint64_t, std::uint64_t, std::uint32_t);
inline constinit std::atomic<UnsafeCallback> published_unsafe{nullptr};
inline constinit std::atomic<ProtectionBeginCallback> published_protection_begin{nullptr};
inline constinit std::atomic<ProtectionEndCallback> published_protection_end{nullptr};
inline constinit std::atomic<ClampedProtectionCallback> published_clamped_protection{nullptr};
#ifndef NDEBUG
using TaggedUnsafeCallback = void (*)(std::uint64_t, std::uint64_t, std::uint32_t);
inline constinit std::atomic<TaggedUnsafeCallback> published_tagged_unsafe{nullptr};
inline bool PoisonTraceRequested() {
    static const bool enabled = [] {
        const char* value = std::getenv("RYGT_SNAPSHOT_POISON_TRACE");
        return value && value[0] == '1' && !value[1];
    }();
    return enabled;
}
#endif
inline constinit std::atomic<bool> protocol_initialized{false};
inline constinit std::atomic<bool> early_unsafe{false};
inline constinit std::atomic<bool> modules_changed{false};
inline constinit std::atomic<std::uint32_t> driver_vendor{0};
inline constinit std::atomic<bool> driver_ready{false};
#ifdef _WIN32
inline constinit std::atomic<HMODULE> driver_module{nullptr};
inline constinit std::atomic<std::uint64_t> driver_next_poll{0};
inline constinit std::atomic<std::uint64_t> driver_first_poll{0};
#endif
// PERF-GTS-424: some NVIDIA drivers (seen on 32.0.16.1742) never load nvrtum64.dll into the
// game, so FIX-354's wait never ended and reuse never started. After this many ms of polling
// without it, reuse is admitted anyway; if the DLL loads later, the module guard revokes reuse
// and snapshots go back to copying. RYGT_SNAPSHOT_DRIVER_GRACE_MS=0 waits forever (FIX-354).
inline constinit std::atomic<bool> driver_grace_used{false};
inline std::uint64_t DriverGraceMs() {
    static const std::uint64_t ms = [] {
        const char* value = std::getenv("RYGT_SNAPSHOT_DRIVER_GRACE_MS");
        return value && *value ? std::strtoull(value, nullptr, 10) : std::uint64_t{30000};
    }();
    return ms;
}
inline bool DriverReady() {
#ifdef _WIN32
    const auto vendor = driver_vendor.load(std::memory_order_acquire);
    if (!vendor) return false;
    if (vendor != 0x10de) return true;
    if (driver_ready.load(std::memory_order_acquire)) return true;
    const auto now = GetTickCount64();
    auto next = driver_next_poll.load(std::memory_order_relaxed);
    if (now < next || !driver_next_poll.compare_exchange_strong(next, now + 100, std::memory_order_relaxed)) return false;
    std::uint64_t first = 0;
    driver_first_poll.compare_exchange_strong(first, now, std::memory_order_relaxed);
    if (!first) first = now;
    HMODULE module{};
    const auto error = GetLastError();
    const bool found = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"nvrtum64.dll", &module) != FALSE;
    SetLastError(error);
    if (!found) {
        const auto grace = DriverGraceMs();
        if (!grace || now - first < grace) return false;
        driver_grace_used.store(true, std::memory_order_release);
        driver_ready.store(true, std::memory_order_release);
        return true;
    }
    driver_module.store(module, std::memory_order_release);
    driver_ready.store(true, std::memory_order_release);
    return true;
#else
    return false;
#endif
}
// DIAG-GTS-423: the first revoking module is recorded in Release too.
#if defined(_WIN32)
// LDR_DLL_NOTIFICATION_DATA has identical loaded/unloaded member layouts.
struct ModuleNoticeString { USHORT length; USHORT maximum_length; const wchar_t* buffer; };
struct ModuleNoticeData { ULONG flags; const ModuleNoticeString* full_name; const ModuleNoticeString* base_name; void* base; ULONG size; };
static_assert(offsetof(ModuleNoticeString, buffer) == sizeof(void*));
static_assert(offsetof(ModuleNoticeData, base) == 3 * sizeof(void*));
static_assert(offsetof(ModuleNoticeData, size) == 4 * sizeof(void*));
struct FirstModuleNotice { ULONG reason{}; std::uintptr_t base{}; ULONG size{}; wchar_t name[128]{}; };
inline constinit std::atomic<unsigned> first_module_state{0}; // 0 empty, 1 writing, 2 published
inline constinit FirstModuleNotice first_module_notice{};
static_assert(std::atomic<unsigned>::is_always_lock_free);
inline bool ReadFirstModuleNotice(FirstModuleNotice& out) noexcept {
    if (first_module_state.load(std::memory_order_acquire) != 2) return false;
    out = first_module_notice;
    return true;
}
#endif
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<UnsafeCallback>::is_always_lock_free);

// Optional protocol: old runtimes retain the exact-byte comparison path. All
// functional hooks share the runtime's bitmap, including writes made before
// the first snapshot is admitted. No C++ object crosses the DLL boundary.
inline bool Requested() {
    static const bool requested = [] {
        const char* value = std::getenv("RYGT_NATIVE_SNAPSHOT_LEASE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return requested;
}

inline bool PrototypeCompatible() {
    static const bool compatible = [] {
        const char* inplace = std::getenv("SHADGT_PROTO_INPLACE");
        const char* import = std::getenv("SHADGT_PROTO_IMPORT");
        return !(inplace && inplace[0] == '1') &&
               !(import && (import[0] == '1' || import[0] == '2' || import[0] == '3'));
    }();
    return compatible;
}

struct BackingProtocol {
    using VersionFn = std::uint32_t (*)();
    using StateFn = int (*)(std::uint64_t*, std::uint64_t*);
    using QueryFn = int (*)(std::uint64_t, std::uint64_t, std::uint64_t*);
    using MarkFn = void (*)(std::uint64_t, std::uint64_t);
    using SafeFn = int (*)(std::uint64_t, std::uint64_t);
    using AliasFn = void* (*)(std::uint64_t, std::uint64_t);
    using PinFn = void* (*)(std::uint64_t, std::uint64_t);
    using ReleasePinFn = void (*)(void*);
    using ProtectFn = ClampedProtectionCallback;

    VersionFn version{};
    StateFn state{};
    QueryFn query{};
    MarkFn mark{};
    SafeFn safe{};
    AliasFn renderer_alias{};
    ProtectionBeginCallback protection_begin{};
    ProtectionEndCallback protection_end{};
    PinFn pin{};
    ReleasePinFn release_pin{};
    ProtectFn protect_clamped{};
#ifndef NDEBUG
    TaggedUnsafeCallback tagged_mark{};
    using CausesFn = std::uint32_t (*)(std::uint64_t, std::uint64_t);
    CausesFn causes{};
#endif

#ifdef _WIN32
    static FARPROC Export(HMODULE module, const char* raw, const char* nid) {
        if (!module) return nullptr;
        if (const auto fn = GetProcAddress(module, raw)) return fn;
        return GetProcAddress(module, nid);
    }
#endif

    static const BackingProtocol& Get() {
        static const BackingProtocol api = [] {
            BackingProtocol result{};
#ifdef _WIN32
            const DWORD previous_error = GetLastError();
            if (const HMODULE module = GetModuleHandleA("libc.prx")) {
                result.version = reinterpret_cast<VersionFn>(Export(module, "RytySnapshotBackingVersion", "+nW6haSKlI8"));
                result.state = reinterpret_cast<StateFn>(Export(module, "RytySnapshotBackingState", "NtSnNcwBLfE"));
                result.query = reinterpret_cast<QueryFn>(Export(module, "RytySnapshotQueryUniqueBacking", "k1uonarmeTM"));
                result.mark = reinterpret_cast<MarkFn>(Export(module, "RytySnapshotMarkUnsafe", "hZxVJ2Jdlec"));
                result.safe = reinterpret_cast<SafeFn>(Export(module, "RytySnapshotRangeSafe", "BBa3HIGOhMQ"));
                result.renderer_alias = reinterpret_cast<AliasFn>(Export(module, "RytySnapshotMapRendererAlias", "KY9dH6tnK5g"));
                result.protection_begin = reinterpret_cast<ProtectionBeginCallback>(
                    Export(module, "RytySnapshotProtectionBegin", "uvy9PZ1vPHI"));
                result.protection_end = reinterpret_cast<ProtectionEndCallback>(
                    Export(module, "RytySnapshotProtectionEnd", "fU9xS-eB9Aw"));
                result.pin = reinterpret_cast<PinFn>(
                    Export(module, "RytySnapshotTryPinReadable", "4R1RYPejFqU"));
                result.release_pin = reinterpret_cast<ReleasePinFn>(
                    Export(module, "RytySnapshotReleasePin", "8vcYjOrVMFM"));
                result.protect_clamped = reinterpret_cast<ProtectFn>(
                    Export(module, "RytySnapshotProtectClamped", "YGext8tkAkY"));
#ifndef NDEBUG
                result.tagged_mark = reinterpret_cast<TaggedUnsafeCallback>(
                    Export(module, "RytySnapshotMarkUnsafeTagged", "zTrmAFyR-+A"));
                result.causes = reinterpret_cast<CausesFn>(
                    Export(module, "RytySnapshotUnsafeCauses", "nRxz39q9nH8"));
#endif
            }
            SetLastError(previous_error);
#endif
            return result;
        }();
        return api;
    }

    // Separate DLL markers are required. A kernel-only marker cannot cover
    // libc, networking and save-data wrappers that invoke host reads directly.
    static bool WriterCapabilities() {
#ifdef _WIN32
        const DWORD previous_error = GetLastError();
        const auto supports = [](const char* module_name, const char* name, const char* nid) {
            const HMODULE module = GetModuleHandleA(module_name);
            const auto fn = reinterpret_cast<VersionFn>(Export(module, name, nid));
            return !module || (fn && fn() == 1);
        };
        const bool supported =
            supports("libkernel.prx", "RytySnapshotHostWritesKernelV1", "xrVbTBGMO98") &&
            supports("libc.prx", "RytySnapshotHostWritesLibcV1", "1PNWWmt30Ew") &&
            supports("libSceNet.prx", "RytySnapshotHostWritesSceNetV1", "u+npHXK3chQ") &&
            supports("libSceSaveData.prx", "RytySnapshotHostWritesSceSaveDataV1", "v7NkTRuCYBc") &&
            supports("libSceSaveData.native.prx", "RytySnapshotHostWritesSceSaveDataNativeV1", "txJR8y9NCck");
        SetLastError(previous_error);
        return supported;
#else
        return false;
#endif
    }

    // Bridge selection only: trusted renderer aliases are safe to create once
    // pre-write poisoning is published, even while reuse admission is deferred.
    bool AliasBridgeAvailable() const {
        return protocol_initialized.load(std::memory_order_acquire) && Requested() &&
               version && version() == 2 && mark && renderer_alias &&
               published_unsafe.load(std::memory_order_acquire) == mark;
    }

    bool Available() const {
        return protocol_initialized.load(std::memory_order_acquire) &&
               !early_unsafe.load(std::memory_order_seq_cst) &&
               !modules_changed.load(std::memory_order_seq_cst) &&
               Requested() && PrototypeCompatible() && DriverReady() && version && state && query && mark && safe && renderer_alias &&
               protection_begin && protection_end && pin && release_pin && protect_clamped && version() == 2;
    }

    static void Initialize(std::uint32_t vendor) {
        auto previous = std::uint32_t{0};
        if (!vendor || (!driver_vendor.compare_exchange_strong(previous, vendor, std::memory_order_acq_rel) && previous != vendor))
            modules_changed.store(true, std::memory_order_seq_cst);
        // Called on the initialization thread, before creating the native device
        // or installing watches. Fault handlers never perform lazy resolution.
        const auto& api = Get();
        if (Requested()) {
            // Also initialize the runtime's environment gate outside fault
            // handling, even when the optional protocol is not yet supported.
            std::uint64_t epoch{}, active{};
            if (api.state) api.state(&epoch, &active);
            if (api.protection_begin && api.protection_end) {
                published_protection_begin.store(api.protection_begin, std::memory_order_release);
                published_protection_end.store(api.protection_end, std::memory_order_release);
            }
            if (api.version && api.version() == 2 && api.protect_clamped)
                published_clamped_protection.store(api.protect_clamped, std::memory_order_release);
#ifndef NDEBUG
            if (PoisonTraceRequested())
                published_tagged_unsafe.store(api.tagged_mark, std::memory_order_release);
#endif
            published_unsafe.store(api.mark, std::memory_order_release);
        }
        protocol_initialized.store(true, std::memory_order_release);
    }

#ifdef _WIN32
    static void CALLBACK ModuleNotification(ULONG reason, const void* data, void*) {
        // Loader callback: no allocations, locks, logging or calls into another
        // DLL. Any module change permanently returns this experiment to copying.
        modules_changed.store(true, std::memory_order_seq_cst);
#if 1 // DIAG-GTS-423: recorded in Release too (no allocation, lock or call)
        unsigned expected = 0;
        if (first_module_state.compare_exchange_strong(expected, 1, std::memory_order_relaxed)) {
            first_module_notice.reason = reason;
            if (data) {
                const auto* notice = static_cast<const ModuleNoticeData*>(data);
                first_module_notice.base = reinterpret_cast<std::uintptr_t>(notice->base);
                first_module_notice.size = notice->size;
                if (notice->base_name && notice->base_name->buffer) {
                    const auto count = notice->base_name->length / sizeof(wchar_t);
                    for (unsigned i = 0; i < count && i < 127; ++i)
                        first_module_notice.name[i] = notice->base_name->buffer[i];
                }
            }
            first_module_state.store(2, std::memory_order_release);
        }
#else
        (void)reason; (void)data;
#endif
    }
#endif

    static bool GuardWriterLifetime() {
        // Do not instantiate the one-time guard while the driver is still cold.
        if (!DriverReady()) return false;
        static const bool guarded = [] {
#ifdef _WIN32
            if (!Get().Available()) return false;
            HMODULE pinned{};
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN |
                                       GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                   reinterpret_cast<LPCSTR>(&ModuleNotification), &pinned))
                return false;
            using RegisterFn = LONG (NTAPI*)(ULONG, void (CALLBACK*)(ULONG, const void*, void*),
                                            void*, void**);
            const HMODULE ntdll = GetModuleHandleA("ntdll.dll");
            const auto register_notification = ntdll ? reinterpret_cast<RegisterFn>(
                GetProcAddress(ntdll, "LdrRegisterDllNotification")) : nullptr;
            static void* cookie{};
            if (!register_notification ||
                register_notification(0, &ModuleNotification, nullptr, &cookie) < 0)
                return false;
            // Close the pre-registration unload race; any later unload revokes.
            if (driver_vendor.load(std::memory_order_acquire) == 0x10de &&
                GetModuleHandleW(L"nvrtum64.dll") != driver_module.load(std::memory_order_acquire)) return false;
            // Register first: a module arriving during capability attestation
            // must also revoke the proof, before that module is initialized.
            for (const char* name : {"libc.prx", "libkernel.prx", "libSceNet.prx",
                                     "libSceSaveData.prx", "libSceSaveData.native.prx"}) {
                // Absent writers cannot run; any later DLL load revokes the
                // protocol before dynamic linking, including audited writers.
                if (GetModuleHandleA(name) &&
                    !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, name, &pinned))
                    return false;
            }
            return WriterCapabilities() &&
                   !modules_changed.load(std::memory_order_seq_cst);
#else
            return false;
#endif
        }();
        return guarded && !modules_changed.load(std::memory_order_seq_cst);
    }

    Mapping ReadMapping(bool unique_backing) const {
        std::uint64_t epoch{}, active{};
        const bool ok = Available() && state(&epoch, &active) && !active && epoch;
        return {epoch, ok, ok, unique_backing};
    }

    Mapping Query(Range range) const {
        std::uint64_t epoch{};
        const bool ok = Available() && GuardWriterLifetime() &&
                        query(range.address, range.size, &epoch) && epoch;
        return {epoch, ok, ok, ok};
    }
};

struct RuntimePoison {
    bool Safe(Range range) const {
        const auto& api = BackingProtocol::Get();
        return api.Available() && api.safe(range.address, range.size) != 0;
    }
    void MarkUnsafe(Range range, std::uint32_t cause = 128) const {
        if (!range.size) return;
#ifndef NDEBUG
        if (const auto callback = published_tagged_unsafe.load(std::memory_order_acquire)) {
            callback(range.address, range.size, cause);
            return;
        }
#endif
        if (const auto callback = published_unsafe.load(std::memory_order_acquire)) {
            callback(range.address, range.size);
        } else {
            early_unsafe.store(true, std::memory_order_seq_cst);
        }
    }
};

inline void MarkUnsafe(std::uint64_t address, std::uint64_t size, std::uint32_t cause = 64) {
    RuntimePoison{}.MarkUnsafe({address, size}, cause);
}

class ProtectionTransition {
    ProtectionEndCallback end{};
public:
    ProtectionTransition(std::uint64_t address, std::uint64_t size) {
        const auto begin = published_protection_begin.load(std::memory_order_acquire);
        const auto finish = published_protection_end.load(std::memory_order_acquire);
        if (begin && finish && begin()) end = finish;
        else MarkUnsafe(address, size, 4);
    }
    ~ProtectionTransition() { if (end) end(); }
    ProtectionTransition(const ProtectionTransition&) = delete;
    ProtectionTransition& operator=(const ProtectionTransition&) = delete;
};

} // namespace Vulkan::SnapshotLease
