// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32) && defined(_M_X64) && defined(__clang__)
#include <immintrin.h>
#include <intrin.h>
#endif

namespace Native {
using SnapshotCompareFn = int (*)(const void*, const void*, std::size_t);

#if defined(_WIN32) && defined(_M_X64) && defined(__clang__)
// Equality only: no reads outside [0, size), including the scalar tail.
__attribute__((target("avx2"))) inline int SnapshotEqualAvx2(const void* lhs, const void* rhs,
                                                             std::size_t size) {
    if (!size) return 0;
    if (size < 4096) return std::memcmp(lhs, rhs, size);
    auto a = static_cast<const std::uint8_t*>(lhs);
    auto b = static_cast<const std::uint8_t*>(rhs);
    while (size >= 128) {
        __m256i difference = _mm256_setzero_si256();
        for (std::size_t i = 0; i < 128; i += 32) {
            const auto x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
            const auto y = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
            difference = _mm256_or_si256(difference, _mm256_xor_si256(x, y));
        }
        if (!_mm256_testz_si256(difference, difference)) return 1;
        a += 128; b += 128; size -= 128;
    }
    while (size >= 32) {
        const auto x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a));
        const auto y = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b));
        const auto difference = _mm256_xor_si256(x, y);
        if (!_mm256_testz_si256(difference, difference)) return 1;
        a += 32; b += 32; size -= 32;
    }
    for (std::size_t i = 0; i < size; ++i) if (a[i] != b[i]) return 1;
    return 0;
}

inline bool SnapshotAvx2Available() noexcept {
    int registers[4]{};
    __cpuid(registers, 0);
    if (registers[0] < 7) return false;
    __cpuid(registers, 1);
    constexpr int osxsave = 1 << 27, avx = 1 << 28;
    if ((registers[2] & (osxsave | avx)) != (osxsave | avx)) return false;
    if ((_xgetbv(0) & 0x6) != 0x6) return false; // OS preserves XMM and YMM state.
    __cpuidex(registers, 7, 0);
    return (registers[1] & (1 << 5)) != 0;
}
#endif

inline SnapshotCompareFn SelectSnapshotCompareForCapability(bool enabled, bool avx2) noexcept {
#if defined(_WIN32) && defined(_M_X64) && defined(__clang__)
    if (enabled && avx2) return &SnapshotEqualAvx2;
#else
    (void)enabled; (void)avx2;
#endif
    return &std::memcmp;
}

// Called once at startup. Only use where the result is tested for equality with zero.
inline SnapshotCompareFn SelectSnapshotCompare() noexcept {
    const char* value = std::getenv("RYGT_SNAPSHOT_SIMD_COMPARE");
    if (value && value[0] == '0') return &std::memcmp;
#if defined(_WIN32) && defined(_M_X64) && defined(__clang__)
    return SelectSnapshotCompareForCapability(true, SnapshotAvx2Available());
#else
    return &std::memcmp;
#endif
}
} // namespace Native
