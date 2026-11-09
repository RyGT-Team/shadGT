#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <vector>
#include "ryty_gpu/native/mapping_refresh.h"
#include "video_core/buffer_cache/imported_view_lease.h"
#include "video_core/buffer_cache/mapping_readback_guard.h"
#include "common/mapping_writeback_gate.h"
#include <future>
#include <chrono>
#include <array>
#include <random>

TEST(MappingRefresh, PartialRemapKeepsTheOtherGpuPages) {
    struct Region { std::uint64_t begin, end; };
    const std::map<std::uint64_t, Region> regions{{0, {0, 12288}}};
    const auto plan = Native::PlanMappingRefresh(regions, 4096, 4096);
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->size(), 1u);
    EXPECT_EQ((*plan)[0].first, 4096u);
    EXPECT_EQ((*plan)[0].end, 8192u);
    std::vector<int> gpu{11, 22, 33};
    const std::vector<int> new_mapping{44};
    for (auto p = (*plan)[0].first; p < (*plan)[0].end; p += 4096)
        gpu[p / 4096] = new_mapping[(p - (*plan)[0].first) / 4096];
    EXPECT_EQ(gpu, (std::vector<int>{11, 44, 33}));
    EXPECT_FALSE(Native::PlanMappingRefresh(regions, 4097, 4096));
    EXPECT_FALSE(Native::PlanMappingRefresh(regions, UINT64_MAX - 4095, 4096));
}

TEST(MappingRefresh, OldGapImportRemainsLeasedAfterMapInvalidation) {
    int freed = 0;
    auto map_owner = std::make_shared<VideoCore::ImportedViewLease>([&] { ++freed; });
    auto submitted = map_owner;
    map_owner.reset(); // successful remap invalidates the old imported view
    EXPECT_EQ(freed, 0);
    submitted.reset(); // earlier GPU submission completed
    EXPECT_EQ(freed, 1);
}

TEST(MappingRefresh, OldAndGapReadbacksCannotCompleteIntoRemappedPages) {
    VideoCore::MappingReadbackGuard guard;
    bool old_valid = true, gap_valid = true;
    guard.BeginUnmap(4096, 4096);
    EXPECT_FALSE(guard.MayComplete(4096, 4));
    EXPECT_TRUE(guard.MayComplete(8192, 4));
    guard.PublishMap(4096, 4096, [&] {
        EXPECT_FALSE(guard.MayComplete(4096, 4));
        old_valid = gap_valid = false;
    });
    EXPECT_TRUE(guard.MayComplete(4096, 4));
    EXPECT_FALSE(old_valid);
    EXPECT_FALSE(gap_valid);
}

TEST(MappingRefresh, MappingTokensRejectOldAndGapWritebacksButKeepOtherRanges) {
    Common::MappingWritebackGate gate;
    gate.FreezeProtocol(true);
    auto old = gate.Capture(4096, 4096);
    auto unrelated = gate.Capture(16384, 4096);
    gate.BeginUnmap(4096, 4096);
    auto gap = gate.Capture(4096, 4096);
    gate.PublishMap(4096, 4096);
    auto fresh = gate.Capture(4096, 4096);
    unsigned writes = 0;
    EXPECT_FALSE(gate.Complete(old, [&] { ++writes; }));
    EXPECT_FALSE(gate.Complete(gap, [&] { ++writes; }));
    EXPECT_TRUE(gate.Complete(unrelated, [&] { ++writes; }));
    EXPECT_TRUE(gate.Complete(fresh, [&] { ++writes; }));
    EXPECT_EQ(writes, 2u);
}

TEST(MappingRefresh, ProtocolCannotEnableAfterAnUnguardedCapture) {
    Common::MappingWritebackGate gate;
    auto early = gate.Capture(4096, 4);
    gate.FreezeProtocol(true); // a late marker cannot alter the process policy
    EXPECT_FALSE(gate.ProtocolAvailable());
    bool written = false;
    EXPECT_TRUE(gate.Complete(early, [&] { written = true; }));
    EXPECT_TRUE(written);
}

TEST(MappingRefresh, UnmapWaitsForAWriteAlreadyInProgress) {
    Common::MappingWritebackGate gate;
    gate.FreezeProtocol(true);
    auto token = gate.Capture(4096, 4096);
    std::promise<void> entered, release;
    auto release_future = release.get_future().share();
    auto writer = std::async(std::launch::async, [&] {
        return gate.Complete(token, [&] { entered.set_value(); release_future.wait(); });
    });
    entered.get_future().wait();
    auto unmap = std::async(std::launch::async, [&] { gate.BeginUnmap(4096, 4096); });
    EXPECT_EQ(unmap.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    release.set_value();
    EXPECT_TRUE(writer.get());
    unmap.get();
    EXPECT_FALSE(gate.Complete(token, [] {}));
}

TEST(MappingRefresh, SerializedInnerNotificationInvalidatesAfterOverlappingEarlyNotifications) {
    Common::MappingWritebackGate gate;
    gate.FreezeProtocol(true);
    gate.BeginUnmap(4096, 4096); // A's outer notification
    gate.BeginUnmap(4096, 4096); // B's outer notification
    gate.BeginUnmap(4096, 4096); // A holds the Ryty registry mutation lock
    gate.PublishMap(4096, 4096);
    auto after_a = gate.Capture(4096, 4);
    gate.BeginUnmap(4096, 4096); // B now owns the mutation lock, before OS replacement
    gate.PublishMap(4096, 4096);
    EXPECT_FALSE(gate.Complete(after_a, [] {}));
    auto after_b = gate.Capture(4096, 4);
    EXPECT_TRUE(gate.Complete(after_b, [] {}));
}

TEST(MappingRefresh, SerializedTransitionsPublishBeforeNextPreNotification) {
    Common::MappingWritebackGate gate;
    gate.FreezeProtocol(true);
    std::recursive_mutex transition;
    std::promise<void> first_pre, release_first, second_started;
    auto release = release_first.get_future().share();
    auto first = std::async(std::launch::async, [&] {
        std::lock_guard lock{transition};
        gate.BeginUnmap(4096, 4096);
        first_pre.set_value();
        release.wait();
        { std::lock_guard nested{transition}; } // ReleaseFixedOverlap -> DoMunmap
        gate.PublishMap(4096, 4096);
    });
    first_pre.get_future().wait();
    auto second = std::async(std::launch::async, [&] {
        second_started.set_value();
        std::lock_guard lock{transition};
        gate.BeginUnmap(4096, 4096);
        gate.PublishMap(4096, 4096);
    });
    second_started.get_future().wait();
    EXPECT_EQ(second.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    auto gap = gate.Capture(4096, 4);
    release_first.set_value();
    first.get();
    second.get();
    EXPECT_FALSE(gate.Complete(gap, [] {}));
    EXPECT_TRUE(gate.Complete(gate.Capture(4096, 4), [] {}));
}

TEST(MappingRefresh, AdjacentOverlapsAndRemappedHolesRemainIndependent) {
    Common::MappingWritebackGate gate;
    gate.FreezeProtocol(true);
    gate.BeginUnmap(20, 10);
    gate.BeginUnmap(10, 10);
    gate.BeginUnmap(25, 15);
    gate.BeginUnmap(60, 10);
    gate.PublishMap(18, 15);
    const auto can_write = [&](std::uint64_t start, std::uint64_t size) {
        return gate.Complete(gate.Capture(start, size), [] {});
    };
    EXPECT_FALSE(can_write(10, 8));
    EXPECT_TRUE(can_write(18, 15));
    EXPECT_FALSE(can_write(32, 2));
    EXPECT_TRUE(can_write(40, 20));
    EXPECT_FALSE(can_write(60, 1));
    EXPECT_TRUE(can_write(70, 1));
}

TEST(MappingRefresh, RandomizedRangesMatchByteReference) {
    Common::MappingWritebackGate gate;
    gate.FreezeProtocol(true);
    std::array<bool, 256> unavailable{};
    std::mt19937 random{0x326};
    for (unsigned iteration = 0; iteration < 5000; ++iteration) {
        const unsigned begin = random() % unavailable.size();
        const unsigned size = 1 + random() % (unavailable.size() - begin);
        switch (random() % 3) {
        case 0:
            gate.BeginUnmap(begin, size);
            std::fill(unavailable.begin() + begin, unavailable.begin() + begin + size, true);
            break;
        case 1:
            gate.PublishMap(begin, size);
            std::fill(unavailable.begin() + begin, unavailable.begin() + begin + size, false);
            break;
        default: {
            const bool expected = std::none_of(unavailable.begin() + begin,
                                               unavailable.begin() + begin + size,
                                               [](bool value) { return value; });
            EXPECT_EQ(gate.Complete(gate.Capture(begin, size), [] {}), expected)
                << "iteration " << iteration << " range " << begin << "+" << size;
        }
        }
    }
}
