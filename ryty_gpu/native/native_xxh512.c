// SPDX-License-Identifier: GPL-2.0-or-later
// AVX-512 is reached only after xxHash verifies CPU and OS vector state.
// On non-x86 targets these wrappers stay on the ordinary xxHash path.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include "xxh_x86dispatch.c"
int NativeXXH512Available(void) { return XXH_featureTest() == XXH_AVX512; }
XXH64_hash_t NativeXXH512Long(const void* p, size_t n) { return XXHL64_default_avx512(p,n); }
XXH64_hash_t NativeXXH512LongSeed(const void* p, size_t n, XXH64_hash_t seed) { return XXHL64_seed_avx512(p,n,seed); }
#else
#include <xxhash.h>
int NativeXXH512Available(void) { return 0; }
XXH64_hash_t NativeXXH512Long(const void* p, size_t n) { return XXH3_64bits(p,n); }
XXH64_hash_t NativeXXH512LongSeed(const void* p, size_t n, XXH64_hash_t seed) { return XXH3_64bits_withSeed(p,n,seed); }
#endif
