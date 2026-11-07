// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstdlib>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

#include "video_core/renderer_vulkan/snapshot_source_lease.h"

namespace Vulkan::SnapshotLease {
using UnsafeCallback = void (*)(std::uint64_t, std::uint64_t);
inline constinit std::atomic<UnsafeCallback> published_unsafe{nullptr};
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

struct BackingProtocol {
    using VersionFn = std::uint32_t (*)();
    using StateFn = int (*)(std::uint64_t*, std::uint64_t*);
    using QueryFn = int (*)(std::uint64_t, std::uint64_t, std::uint64_t*);
    using MarkFn = void (*)(std::uint64_t, std::uint64_t);
    using SafeFn = int (*)(std::uint64_t, std::uint64_t);
    using AliasFn = void* (*)(std::uint64_t, std::uint64_t);

    VersionFn version{};
    StateFn state{};
    QueryFn query{};
    MarkFn mark{};
    SafeFn safe{};
    AliasFn renderer_alias{};
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
            return fn && fn() == 1;
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

    bool Available() const {
        return protocol_initialized.load(std::memory_order_acquire) &&
               !early_unsafe.load(std::memory_order_seq_cst) &&
               !modules_changed.load(std::memory_order_seq_cst) &&
               Requested() && version && state && query && mark && safe && renderer_alias && version() == 1;
    }

    static void Initialize() {
        // Called on the initialization thread, before creating the native device
        // or installing watches. Fault handlers never perform lazy resolution.
        const auto& api = Get();
        if (Requested()) {
            // Also initialize the runtime's environment gate outside fault
            // handling, even when the optional protocol is not yet supported.
            std::uint64_t epoch{}, active{};
            if (api.state) api.state(&epoch, &active);
#ifndef NDEBUG
            if (PoisonTraceRequested())
                published_tagged_unsafe.store(api.tagged_mark, std::memory_order_release);
#endif
            published_unsafe.store(api.mark, std::memory_order_release);
        }
        protocol_initialized.store(true, std::memory_order_release);
    }

#ifdef _WIN32
    static void CALLBACK ModuleNotification(ULONG, const void*, void*) {
        // Loader callback: no allocations, locks, logging or calls into another
        // DLL. Any module change permanently returns this experiment to copying.
        modules_changed.store(true, std::memory_order_seq_cst);
    }
#endif

    static bool GuardWriterLifetime() {
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
            // Register first: a module arriving during capability attestation
            // must also revoke the proof, before that module is initialized.
            for (const char* name : {"libc.prx", "libkernel.prx", "libSceNet.prx",
                                     "libSceSaveData.prx", "libSceSaveData.native.prx"}) {
                if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, name, &pinned))
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

} // namespace Vulkan::SnapshotLease
