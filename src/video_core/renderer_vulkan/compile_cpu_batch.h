#pragma once
namespace Vulkan {
// Compiler fixes are production behavior, independent of launcher environment masks.
constexpr unsigned CompileCpuBatchMask() { return 15u; }
constexpr bool CompileCpuBatchEnabled(unsigned bit) { return (15u & bit) != 0; }
}
