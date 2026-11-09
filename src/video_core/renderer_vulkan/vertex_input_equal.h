// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstring>
#include <type_traits>
namespace Vulkan {
// Byte equality is a sufficient (conservative) test for integer/pointer Vulkan
// vertex descriptions. Different padding can only cause an extra state bind.
template<class A, class B> bool VertexInputBytesEqual(const A& a, const B& b) {
    using T = typename A::value_type;
    static_assert(std::is_same_v<T, typename B::value_type>);
    static_assert(std::is_trivially_copyable_v<T>);
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}
} // namespace Vulkan
