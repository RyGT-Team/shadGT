// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdexcept>
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/speculative_key.h"

namespace Common {
int& RecoverableDepth() { static thread_local int depth = 0; return depth; }
}

TEST(SpeculativeKey, RecoverableFailureDropsStateAndRestoresNestedOverride) {
    const int original = 1, stale = 2;
    const int* current = &original;
    EXPECT_FALSE(Vulkan::TrySpeculativeKey(current, &stale, [&] {
        EXPECT_EQ(current, &stale);
        EXPECT_EQ(Common::RecoverableDepth(), 1);
        throw Common::RecoverableFailure{"recycled shader"};
        return true;
    }));
    EXPECT_EQ(current, &original);
    EXPECT_EQ(Common::RecoverableDepth(), 0);
}

TEST(SpeculativeKey, SuccessAndOtherExceptionsRestorePriorState) {
    const int original = 1, speculative = 2;
    const int* current = &original;
    EXPECT_TRUE(Vulkan::TrySpeculativeKey(current, &speculative, [&] { return current == &speculative; }));
    EXPECT_EQ(current, &original);
    EXPECT_THROW(Vulkan::TrySpeculativeKey(current, &speculative, []() -> bool {
        throw std::runtime_error{"unexpected"};
    }), std::runtime_error);
    EXPECT_EQ(current, &original);
    EXPECT_EQ(Common::RecoverableDepth(), 0);
}
