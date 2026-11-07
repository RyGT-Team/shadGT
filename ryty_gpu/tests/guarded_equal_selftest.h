// SPDX-License-Identifier: GPL-2.0-or-later
// Included inside Core::RytyMemory after the production GuardedEqual definition.
extern "C" __declspec(dllexport) int RytyGuardedEqualSelfTest(char* message, int capacity) {
    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    const size_t page = sys.dwPageSize;
    auto* guest = static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 3, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* stable = static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 3, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    bool remap_tested = false;
    const char* result = [&]() -> const char* {
        if (!guest || !stable) return "allocation failed";
        for (size_t i = 0; i < page * 2; ++i) guest[i] = stable[i] = static_cast<unsigned char>(i * 17);
        bool equal = false;
        const int original_depth = g_guarded_reads;
        if (!GuardedEqual(nullptr, nullptr, 0, equal) || !equal || g_guarded_reads != original_depth) return "zero-length null comparison";
        if (!GuardedEqual(guest, stable, page * 2, equal) || !equal || g_guarded_reads != original_depth) return "equal comparison";
        for (const size_t offset : {size_t{0}, page, page * 2 - 1}) {
            guest[offset] ^= 1;
            equal = true;
            if (!GuardedEqual(guest, stable, page * 2, equal) || equal || g_guarded_reads != original_depth) return "mismatch comparison";
            guest[offset] ^= 1;
        }
        DWORD previous{};
        if (!VirtualProtect(guest + page * 2, page, PAGE_NOACCESS, &previous) ||
            !VirtualProtect(stable + page * 2, page, PAGE_NOACCESS, &previous)) return "protect boundary failed";
        for (const size_t length : {size_t{4095}, size_t{4096}, size_t{4097}, size_t{4127}, size_t{4159}}) {
            if (length + 7 > page * 2) continue;
            auto* a = guest + page * 2 - length;
            auto* b = stable + page * 2 - length - 7;
            std::memcpy(b, a, length);
            if (!GuardedEqual(a, b, length, equal) || !equal || g_guarded_reads != original_depth) return "boundary equal comparison";
            for (const size_t offset : {size_t{0}, length / 2, length - 1}) {
                b[offset] ^= 1;
                const bool ok = GuardedEqual(a, b, length, equal);
                b[offset] ^= 1;
                if (!ok || equal || g_guarded_reads != original_depth) return "boundary mismatch comparison";
            }
            // Both operands reach a protected-page boundary in one of the calls.
            if (!GuardedEqual(b, a, length, equal) || !equal || g_guarded_reads != original_depth) return "reversed boundary comparison";
        }
        for (size_t i = 0; i < page * 2; ++i) guest[i] = stable[i] = static_cast<unsigned char>(i * 17);
        if (!VirtualProtect(guest + page, page, PAGE_NOACCESS, &previous)) return "protect noaccess failed";
        equal = true;
        const bool ok = GuardedEqual(guest, stable, page * 2, equal);
        DWORD ignored{};
        if (!VirtualProtect(guest + page, page, PAGE_READWRITE, &ignored)) return "restore protection failed";
        if (ok || equal || g_guarded_reads != original_depth) return "noaccess fault or guard depth";
        if (!GuardedEqual(guest, stable, page * 2, equal) || !equal) return "comparison after protection restore";
        if (!VirtualProtect(guest + page, page, PAGE_READWRITE | PAGE_GUARD, &previous)) return "protect guard failed";
        equal = true;
        const bool guarded_ok = GuardedEqual(guest, stable, page * 2, equal);
        if (!VirtualProtect(guest + page, page, PAGE_READWRITE, &ignored)) return "restore guard protection failed";
        if (guarded_ok || equal || g_guarded_reads != original_depth) return "guard-page fault or guard depth";
        auto* old = guest;
        if (!VirtualFree(guest, 0, MEM_RELEASE)) return "release failed";
        guest = static_cast<unsigned char*>(VirtualAlloc(old, page * 3, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (guest == old) {
            remap_tested = true;
            for (size_t i = 0; i < page * 2; ++i) guest[i] = stable[i];
            guest[page] ^= 1;
            equal = true;
            if (!GuardedEqual(guest, stable, page * 2, equal) || equal || g_guarded_reads != original_depth) return "remap changed bytes";
        }
        return nullptr;
    }();
    if (guest) VirtualFree(guest, 0, MEM_RELEASE);
    if (stable) VirtualFree(stable, 0, MEM_RELEASE);
    if (message && capacity > 0) {
        // lstrcpynA always terminates when capacity is positive.
        lstrcpynA(message, result ? result : remap_tested ? "PASS (same-address remap tested)" : "PASS (same-address remap unavailable)", capacity);
    }
    return result ? 1 : 0;
}
