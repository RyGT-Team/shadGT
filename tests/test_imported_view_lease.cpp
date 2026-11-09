// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include "common/types.h"
#include "video_core/buffer_cache/imported_part_plan.h"
#include "video_core/buffer_cache/imported_view_lease.h"

namespace {
struct View { u64 begin, end; std::shared_ptr<int> wrapper; std::shared_ptr<VideoCore::ImportedViewLease> lease; };
struct Part { u64 offset, range_offset, size; std::shared_ptr<VideoCore::ImportedViewLease> lease; };
auto Plan(const std::map<u64, View>& views, u64 address, u64 size, u64 align, u64 size_align) {
    return VideoCore::PlanImportedParts<Part>(views, address, size, align, size_align,
        [](const View& v, u64 off, u64 pos, u64 len) { return Part{off, pos, len, v.lease}; });
}
}
TEST(ImportedViewLease, IncompleteOrMisalignedPlansReturnNoParts) {
    int freed = 0;
    auto lease = std::make_shared<VideoCore::ImportedViewLease>([&] { ++freed; });
    std::map<u64, View> views{{0, {0, 4096, std::make_shared<int>(), lease}},
                              {8192, {8192, 12288, std::make_shared<int>(), lease}}};
    EXPECT_FALSE(Plan(views, 0, 12288, 16, 16)); // missing middle part
    EXPECT_FALSE(Plan(views, 1, 16, 16, 16));
    EXPECT_FALSE(Plan(views, 0, 15, 16, 16));
    EXPECT_EQ(freed, 0);
}
TEST(ImportedViewLease, UnmapAndEarlySchedulerTickCannotFreeHeldSubmission) {
    int freed = 0;
    auto view = std::make_shared<VideoCore::ImportedViewLease>([&] { ++freed; });
    std::shared_ptr<void> unmap_gate = view, native_stream = view;
    view.reset(); // map removed
    unmap_gate.reset(); // earlier captured scheduler tick completed
    EXPECT_EQ(freed, 0);
    auto submitted = std::move(native_stream);
    EXPECT_EQ(freed, 0); // CPU replay alone is not GPU completion
    submitted.reset(); // actual splice completion
    EXPECT_EQ(freed, 1);
}

TEST(ImportedViewLease, EnqueueFailureKeepsOwnersAndSuccessfulQueueHoldsUntilCompletion) {
    int freed = 0;
    auto view = std::make_shared<VideoCore::ImportedViewLease>([&] { ++freed; });
    std::vector<std::shared_ptr<void>> leases{view};
    view.reset();
    EXPECT_FALSE(VideoCore::RetireImportedLeases(leases, [](auto) { throw 1; },
                                                [](auto held) { held->clear(); }));
    ASSERT_EQ(leases.size(), 1u);
    EXPECT_EQ(freed, 0);
    std::function<void()> after_gpu;
    EXPECT_TRUE(VideoCore::RetireImportedLeases(leases, [&](auto callback) {
        after_gpu = std::move(callback);
    }, [](auto held) { held->clear(); }));
    EXPECT_TRUE(leases.empty());
    EXPECT_EQ(freed, 0);
    after_gpu();
    EXPECT_EQ(freed, 1);
}

TEST(ImportedViewLease, UnmapRetainsRemovedViewWhenSchedulerEnqueueFails) {
    int freed = 0, invalidated = 0;
    auto lease = std::make_shared<VideoCore::ImportedViewLease>([&] { ++freed; });
    std::map<u64, View> views{{0, {0, 4096, std::make_shared<int>(), lease}}};
    lease.reset();
    std::vector<std::shared_ptr<void>> held;
    VideoCore::OnUnmapImportedViews(views, 0, 4096, held, false,
        [&](View&) { ++invalidated; }, [&](auto& owners) {
            EXPECT_FALSE(VideoCore::RetireImportedLeases(owners, [](auto) { throw 1; },
                [](auto completed) { completed->clear(); }));
        });
    EXPECT_TRUE(views.empty());
    EXPECT_EQ(invalidated, 1);
    ASSERT_EQ(held.size(), 1u);
    EXPECT_EQ(freed, 0);
    std::function<void()> after_gpu;
    EXPECT_TRUE(VideoCore::RetireImportedLeases(held, [&](auto callback) {
        after_gpu = std::move(callback);
    }, [](auto completed) { completed->clear(); }));
    EXPECT_EQ(freed, 0);
    after_gpu();
    EXPECT_EQ(freed, 1);
}
