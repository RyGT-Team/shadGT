// GTS-014: files the game opens can be renamed and deleted while open, as on the PS4.
//
// RyGT's libkernel.prx opens guest files with the C runtime's _wopen, which shares a file for
// reading and writing but not for deleting. On Windows a file opened that way cannot be renamed or
// deleted until every handle to it is closed; on the PS4 (and Linux) it can. GT Sport records a
// race into APP_DATA/replay/replay_a.dat and keeps it open while saving the replay, which renames
// and deletes the recording files, so the save failed on Windows and no replay was kept.
//
// This replaces libkernel.prx's _wopen import with one that opens the file with CreateFileW and
// FILE_SHARE_DELETE as well, then hands the handle to the C runtime, so the descriptor works exactly
// as before. If CreateFileW fails, the original _wopen runs, so errors (errno) stay the same.
// The renames of libstdc++ (std::filesystem::rename, used by sceKernelRename) are logged when they
// fail. RYGT_FILE_SHARE_DELETE=0 turns this off.

#include <windows.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

namespace {

using WopenFn = int(__cdecl*)(const wchar_t*, int, ...);
using WrenameFn = int(__cdecl*)(const wchar_t*, const wchar_t*);
using MoveFileExWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);

WopenFn g_original_wopen = nullptr;
WrenameFn g_original_wrename = nullptr;
MoveFileExWFn g_original_move = nullptr;

int __cdecl SharedWopen(const wchar_t* path, int flags, ...) {
    int mode = 0;
    if (flags & _O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    DWORD access = GENERIC_READ;
    switch (flags & (_O_RDONLY | _O_WRONLY | _O_RDWR)) {
    case _O_WRONLY: access = GENERIC_WRITE; break;
    case _O_RDWR: access = GENERIC_READ | GENERIC_WRITE; break;
    default: break;
    }
    DWORD disposition = OPEN_EXISTING;
    if ((flags & (_O_CREAT | _O_EXCL)) == (_O_CREAT | _O_EXCL)) {
        disposition = CREATE_NEW;
    } else if ((flags & (_O_CREAT | _O_TRUNC)) == (_O_CREAT | _O_TRUNC)) {
        disposition = CREATE_ALWAYS;
    } else if (flags & _O_CREAT) {
        disposition = OPEN_ALWAYS;
    } else if (flags & _O_TRUNC) {
        disposition = TRUNCATE_EXISTING;
    }
    const DWORD attributes =
        (flags & _O_CREAT) && !(mode & _S_IWRITE) ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL;
    HANDLE handle = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, disposition, attributes, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        // Directories, missing files and the like: the C runtime gives the usual error.
        return (flags & _O_CREAT) ? g_original_wopen(path, flags, mode) : g_original_wopen(path, flags);
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
                                   flags & (_O_APPEND | _O_RDONLY | _O_TEXT));
    if (fd == -1) {
        CloseHandle(handle);
        return (flags & _O_CREAT) ? g_original_wopen(path, flags, mode) : g_original_wopen(path, flags);
    }
    if (flags & _O_BINARY) {
        _setmode(fd, _O_BINARY);
    }
    return fd;
}

int __cdecl LoggedWrename(const wchar_t* from, const wchar_t* to) {
    const int result = g_original_wrename(from, to);
    if (result != 0) {
        std::fprintf(stderr, "[patch] GTS-014 rename failed (errno %d): %ls -> %ls\n", errno, from, to);
    }
    return result;
}

BOOL WINAPI LoggedMoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    const BOOL result = g_original_move(from, to, flags);
    if (!result) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "[patch] GTS-014 rename failed (error %lu): %ls -> %ls\n", error, from,
                     to ? to : L"");
        SetLastError(error);
    }
    return result;
}

// Points `module`'s import of `name` at `replacement` and returns the old target.
void* PatchImport(HMODULE module, const char* name, void* replacement) {
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        return nullptr;
    }
    for (auto* entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
         entry->Name != 0; ++entry) {
        if (entry->OriginalFirstThunk == 0) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), name) != 0) {
                continue;
            }
            void* old = reinterpret_cast<void*>(slots->u1.Function);
            DWORD protect = 0;
            VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &protect);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), protect, &protect);
            return old;
        }
    }
    return nullptr;
}

} // namespace

void InstallFileShareDelete() {
    if (const char* env = std::getenv("RYGT_FILE_SHARE_DELETE"); env && env[0] == '0') {
        return;
    }
    HMODULE kernel = GetModuleHandleA("libkernel.prx");
    if (kernel == nullptr) {
        std::fprintf(stderr, "[patch] GTS-014: libkernel.prx not loaded, files open as before\n");
        return;
    }
    g_original_wopen = static_cast<WopenFn>(PatchImport(kernel, "_wopen", reinterpret_cast<void*>(&SharedWopen)));
    std::fprintf(stderr, "[patch] GTS-014 open files shareable for rename/delete: %s\n",
                 g_original_wopen ? "applied" : "_wopen import not found");
    if (HMODULE cxx = GetModuleHandleA("libstdc++-6.dll")) {
        g_original_wrename = static_cast<WrenameFn>(PatchImport(cxx, "_wrename", reinterpret_cast<void*>(&LoggedWrename)));
        g_original_move = reinterpret_cast<MoveFileExWFn>(
            PatchImport(cxx, "MoveFileExW", reinterpret_cast<void*>(&LoggedMoveFileExW)));
    }
}
