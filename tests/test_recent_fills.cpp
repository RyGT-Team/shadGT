#include <gtest/gtest.h>
#include <stdexcept>
#include "ryty_gpu/native/recent_fills.h"

TEST(RecentFills, FindsCurrentFrameAndRotatesAlternatingPattern) {
    Native::RecentFills fills;
    fills.Record(0x1000, 0x100, {0x11223344, 0x55667788}, 2, 7);
    auto aligned = fills.Find(0x1000, 0x80, 2);
    ASSERT_TRUE(aligned);
    EXPECT_EQ(aligned->pattern, (Native::RecentFills::Pattern{0x11223344, 0x55667788}));
    auto rotated = fills.Find(0x1004, 0x80, 2);
    ASSERT_TRUE(rotated);
    EXPECT_EQ(rotated->pattern, (Native::RecentFills::Pattern{0x55667788, 0x11223344}));
    EXPECT_FALSE(fills.Find(0x1001, 0x80, 2));
    EXPECT_FALSE(fills.Find(0x1000, 0x80, 3));
    EXPECT_FALSE(fills.Find(0x1080, 0x81, 2));
}

TEST(RecentFills, ForgetAndOverflowAreSafe) {
    Native::RecentFills fills;
    fills.Record(0x1000, 0x100, {1, 2}, 1, 1);
    fills.Forget(0x1080, 1);
    EXPECT_FALSE(fills.Find(0x1000, 4, 1));
    fills.Record(~std::uint64_t{0} - 2, 4, {1, 2}, 1, 2);
    EXPECT_FALSE(fills.Find(~std::uint64_t{0} - 2, 4, 1));
}

TEST(RecentFills, NewOverlapSupersedesOldAndFrameExpires) {
    Native::RecentFills fills;
    fills.Record(0x1000, 0x100, {1, 2}, 1, 1);
    fills.Record(0x1080, 0x80, {3, 4}, 1, 2);
    EXPECT_FALSE(fills.Find(0x1000, 4, 1));
    ASSERT_TRUE(fills.Find(0x1080, 4, 1));
    EXPECT_EQ(fills.Find(0x1080, 4, 1)->pattern[0], 3u);
    fills.Expire(2);
    EXPECT_TRUE(fills.Ranges().empty());
}

TEST(RecentFills, SchedulerEnqueueFailureRetainsStreamOwners) {
    auto value = std::make_shared<int>(42);
    std::weak_ptr<int> weak = value;
    std::vector<std::shared_ptr<int>> stream{value};
    value.reset();
    EXPECT_THROW(Native::EnqueueRetainedReceipts(stream, [](auto) {
        throw std::runtime_error("queue failed");
    }), std::runtime_error);
    ASSERT_EQ(stream.size(), 1u);
    EXPECT_FALSE(weak.expired());
    std::vector<std::shared_ptr<int>> queued;
    Native::EnqueueRetainedReceipts(stream, [&](auto copy) { queued = std::move(copy); });
    EXPECT_TRUE(stream.empty());
    EXPECT_FALSE(weak.expired());
    queued.clear();
    EXPECT_TRUE(weak.expired());
}
