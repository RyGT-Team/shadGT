#pragma once
#include <cstdint>

namespace Native {
// Uploads of large CPU-only textures need no persistent host-memory
// import. Preserve the ordered GPU-source path for every GPU-owned/aliased case.
constexpr bool PreferCpuTextureStaging(std::uint64_t size, bool nvidia,
                                      bool from_cpu, bool cpu_only,
                                      bool gpu_written, bool has_gpu_sources,
                                      bool overlaps_region, bool single_import,
                                      bool readable) {
    return nvidia && size > (4ull << 20) && size <= (64ull << 20) &&
           from_cpu && cpu_only && !gpu_written && !has_gpu_sources &&
           !overlaps_region && !single_import && readable;
}
}
