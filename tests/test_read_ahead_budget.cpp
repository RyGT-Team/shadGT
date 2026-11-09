// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/read_ahead_budget.h"

using namespace std::chrono_literals;
TEST(ReadAheadBudget, LimitsOptionalWorkAndExemptsRequiredWaits) {
    Vulkan::ReadAheadBudget budget;
    using Clock = Vulkan::ReadAheadBudget::Clock;
    const auto start = Clock::time_point{} + 100ms;
    ASSERT_TRUE(budget.Deadline(start, false));
    EXPECT_EQ(*budget.Deadline(start, false), start + 2ms);
    budget.Charge(start, start + 2ms, false);
    const auto second = start + 2ms;
    ASSERT_TRUE(budget.Deadline(second, false));
    EXPECT_EQ(*budget.Deadline(second, false), second + 1ms);
    budget.Charge(second, second + 1ms, false);
    EXPECT_FALSE(budget.Deadline(start + 3ms, false));
    ASSERT_TRUE(budget.Deadline(start + 3ms, true));
    EXPECT_EQ(*budget.Deadline(start + 3ms, true), start + 5ms);
    budget.Charge(start + 3ms, start + 10ms, true);
    EXPECT_FALSE(budget.Deadline(start + 16ms, false));
    ASSERT_TRUE(budget.Deadline(start + 17ms, false));
    EXPECT_EQ(*budget.Deadline(start + 17ms, false), start + 19ms);
}
