#pragma once
#include <cstddef>
#include <cstdint>
#include <xxhash.h>
extern "C" int NativeXXH512Available(void);
extern "C" std::uint64_t NativeXXH512Long(const void*,std::size_t);
extern "C" std::uint64_t NativeXXH512LongSeed(const void*,std::size_t,std::uint64_t);
namespace Native::Parallel {
inline bool XXH512Available() noexcept { static const bool supported=NativeXXH512Available()!=0;return supported; }
inline std::uint64_t AdaptiveXXH3WithCapability(const void* p,std::size_t n,bool supported) noexcept {
    return n>240 && n<=4096 && supported ? NativeXXH512Long(p,n) : XXH3_64bits(p,n);
}
inline std::uint64_t AdaptiveXXH3SeedWithCapability(const void* p,std::size_t n,std::uint64_t seed,bool supported) noexcept {
    return n>240 && n<=384 && supported ? NativeXXH512LongSeed(p,n,seed) : XXH3_64bits_withSeed(p,n,seed);
}
inline std::uint64_t AdaptiveXXH3(const void* p,std::size_t n) noexcept {
    if(n<=240 || n>4096)return XXH3_64bits(p,n);return AdaptiveXXH3WithCapability(p,n,XXH512Available());
}
inline std::uint64_t AdaptiveXXH3Seed(const void* p,std::size_t n,std::uint64_t seed) noexcept {
    if(n<=240 || n>384)return XXH3_64bits_withSeed(p,n,seed);return AdaptiveXXH3SeedWithCapability(p,n,seed,XXH512Available());
}
}
