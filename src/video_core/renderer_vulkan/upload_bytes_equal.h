// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstring>
#if defined(__AVX2__) || defined(_M_AVX2)
#include <immintrin.h>
#endif
namespace Vulkan {
// Equality only: unlike memcmp, no lexicographic result is needed. Every vector
// load stays inside the supplied range, including unaligned and page-end inputs.
inline bool UploadBytesEqual(const void* left, const void* right, std::size_t size) {
    const auto* a = static_cast<const unsigned char*>(left);
    const auto* b = static_cast<const unsigned char*>(right);
#if defined(__AVX2__) || defined(_M_AVX2)
    if (size < 512) return size == 0 || std::memcmp(a, b, size) == 0;
    while (size >= 128) {
        const auto d0 = _mm256_xor_si256(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a)),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b)));
        const auto d1 = _mm256_xor_si256(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + 32)),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + 32)));
        const auto d2 = _mm256_xor_si256(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + 64)),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + 64)));
        const auto d3 = _mm256_xor_si256(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + 96)),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + 96)));
        const auto diff = _mm256_or_si256(_mm256_or_si256(d0, d1), _mm256_or_si256(d2, d3));
        if (!_mm256_testz_si256(diff, diff)) return false;
        a += 128;
        b += 128;
        size -= 128;
    }
    while (size >= 32) {
        const auto x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a));
        const auto y = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b));
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, y)) != -1) return false;
        a += 32;
        b += 32;
        size -= 32;
    }
#endif
    return size == 0 || std::memcmp(a, b, size) == 0;
}
} // namespace Vulkan
