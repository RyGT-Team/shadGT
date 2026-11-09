#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <sstream>
#include <string>

namespace Ryty::Camera::Validation {
struct HookSite { std::uintptr_t slot, original; };
inline constexpr std::array<HookSite, 8> HookSites{{
    {0x2878ab8, 0x86b030}, {0x2878918, 0x8688e0},
    {0x28789e8, 0x869620}, {0x2878848, 0x8683f0},
    {0x2878a68, 0x8a5560}, {0x28788c8, 0x8a5560},
    {0x2878998, 0x8a5560}, {0x28787f8, 0x8a5560},
}};
struct Signature {
    const char* name;
    std::uint32_t rva;
    std::array<unsigned char, 64> bytes;
};
// 64-byte, exact, RVA-relative signatures from the validated CUSA03220 1.69 layout.
inline constexpr std::array<Signature, 9> Signatures{{
    {"Cockpit final view", 0x86b030, {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x18, 0x49, 0x89, 0xcf,
        0x49, 0x89, 0xfe, 0xe8, 0xd8, 0xb2, 0x03, 0x00, 0x41, 0x83, 0xbe, 0x1c, 0x03, 0x00, 0x00, 0x1a,
        0x75, 0x52, 0x41, 0x80, 0xbf, 0xd8, 0x00, 0x00, 0x00, 0x00, 0x75, 0x2c, 0x41, 0x83, 0xbf, 0xdc,
        0x00, 0x00, 0x00, 0x00, 0x7f, 0x22, 0x41, 0x8b, 0x87, 0xc4, 0x00, 0x00, 0x00, 0x01, 0xc0, 0x41,
    }},
    {"Chase final view", 0x8688e0, {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x58, 0x8b, 0x87, 0x18,
        0x03, 0x00, 0x00, 0x48, 0x89, 0xcb, 0x49, 0x89, 0xfe, 0xa8, 0x01, 0x0f, 0x84, 0x20, 0x09, 0x00,
        0x00, 0xc5, 0xfa, 0x10, 0x42, 0x04, 0xc5, 0xfa, 0x10, 0x4a, 0x08, 0xc5, 0x78, 0xc6, 0xf1, 0x41,
        0xa8, 0x02, 0x0f, 0x85, 0x16, 0x09, 0x00, 0x00, 0xa8, 0x04, 0x0f, 0x84, 0x20, 0x09, 0x00, 0x00,
    }},
    {"Bumper final view", 0x869620, {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xcb, 0x49, 0x89, 0xfe, 0xe8, 0xee, 0xcc,
        0x03, 0x00, 0x80, 0xbb, 0xd8, 0x00, 0x00, 0x00, 0x00, 0x75, 0x28, 0x83, 0xbb, 0xdc, 0x00, 0x00,
        0x00, 0x00, 0x7f, 0x1f, 0x8b, 0x83, 0xc4, 0x00, 0x00, 0x00, 0x01, 0xc0, 0x03, 0x83, 0xc8, 0x00,
        0x00, 0x00, 0x48, 0x98, 0x48, 0xc1, 0xe0, 0x07, 0x41, 0x80, 0xbc, 0x06, 0x60, 0x03, 0x00, 0x00,
    }},
    {"Roof final view", 0x8683f0, {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xcb, 0x49, 0x89, 0xfe, 0xe8, 0x1e, 0xdf,
        0x03, 0x00, 0x80, 0xbb, 0xd8, 0x00, 0x00, 0x00, 0x00, 0x75, 0x28, 0x83, 0xbb, 0xdc, 0x00, 0x00,
        0x00, 0x00, 0x7f, 0x1f, 0x8b, 0x83, 0xc4, 0x00, 0x00, 0x00, 0x01, 0xc0, 0x03, 0x83, 0xc8, 0x00,
        0x00, 0x00, 0x48, 0x98, 0x48, 0xc1, 0xe0, 0x07, 0x41, 0x80, 0xbc, 0x06, 0x60, 0x03, 0x00, 0x00,
    }},
    {"Base prepare view", 0x8a5560, {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
        0x28, 0x48, 0x8b, 0x5a, 0x08, 0x49, 0x89, 0xfe, 0x49, 0x89, 0xcd, 0x49, 0x89, 0xd7, 0x49, 0x89,
        0xf4, 0xc5, 0xf8, 0x29, 0x45, 0xc0, 0x48, 0x8b, 0x03, 0x48, 0x89, 0xdf, 0xff, 0x90, 0x88, 0x01,
        0x00, 0x00, 0x84, 0xc0, 0x0f, 0x84, 0x54, 0x02, 0x00, 0x00, 0x4c, 0x89, 0x7d, 0xd0, 0x45, 0x8b,
    }},
    {"Common final view", 0x8a6320, {
        0x45, 0x84, 0xc0, 0x75, 0x4a, 0x8b, 0x87, 0x18, 0x03, 0x00, 0x00, 0xa8, 0x01, 0x0f, 0x84, 0xc3,
        0x05, 0x00, 0x00, 0xc5, 0xfa, 0x10, 0x42, 0x04, 0xc5, 0xfa, 0x10, 0x4a, 0x08, 0xc5, 0x78, 0xc6,
        0xc9, 0x41, 0xa8, 0x02, 0x74, 0x0a, 0xc5, 0xfa, 0x10, 0x02, 0xc4, 0x63, 0x31, 0x0c, 0xc8, 0x01,
        0xa8, 0x04, 0xc4, 0x41, 0x10, 0x57, 0xed, 0x74, 0x05, 0xc5, 0x78, 0x10, 0x6a, 0x10, 0xa8, 0x20,
    }},
    {"Scenario name code", 0x866320, {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x49, 0x89, 0xfe, 0xe8, 0x11, 0x16, 0x4c, 0x01, 0x48,
        0x85, 0xc0, 0x74, 0x7b, 0x49, 0x89, 0xc1, 0x31, 0xf6, 0x4c, 0x8d, 0x05, 0x40, 0x11, 0x68, 0x01,
        0x31, 0xc0, 0x31, 0xdb, 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x41, 0x0f, 0xbe, 0x0c, 0x36, 0x89, 0xca, 0x80, 0xc2, 0xd0, 0x80, 0xfa, 0x09, 0x76, 0x4b, 0x89,
    }},
    {"Native camera selector", 0x8657b0, {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x63, 0xc6, 0x48, 0x89,
        0xc6, 0x48, 0xc1, 0xe6, 0x07, 0x4c, 0x8d, 0x04, 0xc6, 0x4a, 0x8b, 0x44, 0x07, 0x30, 0x4c, 0x8b,
        0x60, 0x08, 0x4c, 0x8b, 0x78, 0x10, 0x4d, 0x29, 0xe7, 0x0f, 0x84, 0xc2, 0x00, 0x00, 0x00, 0x4d,
        0x89, 0xf9, 0x31, 0xdb, 0x49, 0xba, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x89,
    }},
    {"Terrain ray cast", 0xa91990, {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
        0x68, 0x4c, 0x8b, 0x25, 0xc8, 0x88, 0xee, 0x01, 0xc5, 0xfc, 0x10, 0x05, 0xd0, 0xea, 0x35, 0x01,
        0x48, 0x89, 0xf3, 0x41, 0xbf, 0xff, 0xff, 0xff, 0xff, 0x49, 0x8b, 0x04, 0x24, 0x48, 0x89, 0x45,
        0xd0, 0x48, 0xc7, 0x45, 0xa0, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x45, 0xa8, 0xff, 0xff, 0xff, 0xff,
    }},
}};

using Inspect = bool (*)(std::uintptr_t, std::uintptr_t, std::size_t, bool);
using Report = std::function<void(const std::string&)>;
inline bool InspectImage(std::uintptr_t image, std::uintptr_t at, std::size_t size, bool executable) {
    if (!image || !at || !size || at > std::numeric_limits<std::uintptr_t>::max() - size) return false;
    for (auto cursor = at, end = at + size; cursor < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || info.Type != MEM_IMAGE ||
            reinterpret_cast<std::uintptr_t>(info.AllocationBase) != image ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const DWORD protection = info.Protect & 0xff;
        const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
            protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const bool code = protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
            protection == PAGE_EXECUTE_WRITECOPY;
        if (!readable || (executable && !code)) return false;
        const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (start > cursor || info.RegionSize > std::numeric_limits<std::uintptr_t>::max() - start ||
            start + info.RegionSize <= cursor) return false;
        cursor = (std::min)(end, start + info.RegionSize);
    }
    return true;
}
inline std::string At(const char* reason, std::uintptr_t rva) {
    std::ostringstream out;
    out << reason << " at RVA 0x" << std::hex << rva;
    return out.str();
}
inline std::string Validate(std::uintptr_t image, std::span<const HookSite> sites,
                            Inspect inspect = InspectImage, const Report& report = {}) {
    const auto read = [&](std::uintptr_t rva, void* output, std::size_t size) {
        if (!image || rva > std::numeric_limits<std::uintptr_t>::max() - image ||
            !inspect(image, image + rva, size, false)) return false;
        std::memcpy(output, reinterpret_cast<void*>(image + rva), size);
        return true;
    };
    IMAGE_DOS_HEADER dos{};
    if (!read(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < sizeof(dos) || dos.e_lfanew > 0x100000)
        return "Unsupported executable: invalid or unreadable DOS header.";
    IMAGE_NT_HEADERS64 nt{};
    if (!read(static_cast<unsigned>(dos.e_lfanew), &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.FileHeader.SizeOfOptionalHeader > 4096 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96)
        return "Unsupported executable: invalid x64 PE headers.";
    const std::uintptr_t table = static_cast<unsigned>(dos.e_lfanew) + sizeof(DWORD) +
        sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    const auto table_end = table + nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    const auto image_size = nt.OptionalHeader.SizeOfImage;
    if (image_size < table_end || nt.OptionalHeader.SizeOfHeaders < table_end ||
        nt.OptionalHeader.SizeOfHeaders > image_size || image > std::numeric_limits<std::uintptr_t>::max() - image_size)
        return "Unsupported executable: invalid image or header bounds.";
    std::array<IMAGE_SECTION_HEADER, 96> sections{};
    if (!read(table, sections.data(), nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER)))
        return "Unsupported executable: unreadable section table.";
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        const auto size = section.Misc.VirtualSize;
        if (!size || section.VirtualAddress < nt.OptionalHeader.SizeOfHeaders ||
            section.VirtualAddress >= image_size || size > image_size - section.VirtualAddress)
            return At("Unsupported executable: invalid section bounds", section.VirtualAddress);
        for (unsigned j = 0; j < i; ++j) {
            const auto& previous = sections[j];
            if (section.VirtualAddress < previous.VirtualAddress + previous.Misc.VirtualSize &&
                previous.VirtualAddress < section.VirtualAddress + size)
                return "Unsupported executable: overlapping PE sections.";
        }
    }
    const auto range = [&](std::uintptr_t rva, std::size_t size, bool executable) {
        if (rva >= image_size || size > image_size - rva) return false;
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            const auto& s = sections[i];
            if (rva < s.VirtualAddress || rva - s.VirtualAddress >= s.Misc.VirtualSize ||
                size > s.Misc.VirtualSize - (rva - s.VirtualAddress)) continue;
            const bool is_code = (s.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            if (!(s.Characteristics & IMAGE_SCN_MEM_READ) || is_code != executable ||
                (executable && (s.Characteristics & IMAGE_SCN_MEM_WRITE))) return false;
            return inspect(image, image + rva, size, executable);
        }
        return false;
    };
    for (const auto& signature : Signatures) {
        if (!range(signature.rva, signature.bytes.size(), true))
            return At((std::string("Unsupported executable: invalid executable section for ") + signature.name).c_str(), signature.rva);
        const auto* code = reinterpret_cast<const unsigned char*>(image + signature.rva);
        for (unsigned i = 0; i < signature.bytes.size(); ++i)
            if (code[i] != signature.bytes[i])
                return At((std::string("Unsupported executable: signature mismatch for ") + signature.name).c_str(), signature.rva + i);
        if (report) report(At((std::string("SIGNATURE PASS: ") + signature.name).c_str(), signature.rva));
    }
    // These are identity vtables and a live global, not objects that must already exist at startup.
    for (const auto rva : {0x2877ae8u, 0x28785f8u, 0x2854348u, 0x2896278u, 0x2bf5e48u})
        if (!range(rva, sizeof(std::uintptr_t), false))
            return At("Unsupported executable: invalid camera data section", rva);
    if (sites.size() != HookSites.size()) return "Camera hook validation: expected all eight hook slots.";
    for (unsigned i = 0; i < sites.size(); ++i) {
        const auto& site = sites[i];
        if (site.slot != HookSites[i].slot || site.original != HookSites[i].original)
            return At("Camera hook validation: unexpected hook layout", site.slot);
        if (site.slot % alignof(void*) || !range(site.slot, sizeof(void*), false))
            return At("Camera hook validation: invalid slot section or memory", site.slot);
        std::uintptr_t target{};
        if (!read(site.slot, &target, sizeof(target))) return At("Camera hook validation: unreadable slot", site.slot);
        if (target != image + site.original) {
            std::ostringstream error;
            error << At("Camera hook conflict", site.slot) << ": expected 0x" << std::hex
                  << image + site.original << ", found 0x" << target
                  << ". Close external camera tools and restart the game.";
            return error.str();
        }
        if (report) report(At(("HOOK VALIDATED " + std::to_string(i + 1) + "/8").c_str(), site.slot) +
            "; target " + At("", site.original));
    }
    return {};
}
} // namespace Ryty::Camera::Validation
