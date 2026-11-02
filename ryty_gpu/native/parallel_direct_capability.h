#pragma once
#include <cstdint>
namespace VideoCore {
class DirectMemory;
bool ParallelDirectViewCoherent(const DirectMemory*, std::uint64_t, std::uint64_t) noexcept;
}