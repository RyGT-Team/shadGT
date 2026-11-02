#pragma once
#include <cstdint>
#include <span>
#include <utility>
namespace VideoCore {
class DirectMemory;
bool ParallelDirectViewsCoherent(const DirectMemory*, std::span<const std::pair<std::uint64_t,std::uint64_t>>) noexcept;
bool ParallelDirectViewCoherent(const DirectMemory*, std::uint64_t, std::uint64_t) noexcept;
}