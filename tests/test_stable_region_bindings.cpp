// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <map>
#include <future>
#include <thread>
#include "video_core/renderer_vulkan/stable_region_bindings.h"
#include "ryty_gpu/native/region_init_gaps.h"
#include "ryty_gpu/native/retired_snapshot_pool.h"
#include "ryty_gpu/native/writeback_plan.h"
#include "video_core/buffer_cache/imported_view_lease.h"
#include "ryty_gpu/native/safe_page_snapshot.h"
#include "ryty_gpu/native/page_download_ranges.h"
#include "video_core/renderer_vulkan/page_watch_tracker.h"

TEST(WritebackPlan, AFailureInLaterRangePublishesNoPartialPlan) {
    std::vector<int> published{99};
    const std::array ranges{1, 2, 3};
    unsigned visited = 0;
    EXPECT_FALSE(Native::PrepareWritebackOperation(published, ranges,
        [&](auto& candidate, int value) {
            ++visited;
            candidate.push_back(value);
            return value != 3;
        }));
    EXPECT_EQ(visited, 3u);
    EXPECT_EQ(published, std::vector<int>{99});
    EXPECT_TRUE(Native::PrepareWritebackOperation(published, ranges,
        [](auto& candidate, int value) { candidate.push_back(value); return true; }));
    EXPECT_EQ(published, (std::vector<int>{1, 2, 3}));
}

TEST(WritebackPlan, SnapshotSurvivesAnEarlierSpliceUntilLastWritebackCompletes) {
    int released = 0;
    auto plan = std::make_shared<VideoCore::ImportedViewLease>([&] { ++released; });
    std::vector<std::shared_ptr<void>> first{plan}, last{plan};
    std::function<void()> first_done, last_done;
    auto complete = [](auto owners) { owners->clear(); };
    ASSERT_TRUE(VideoCore::RetireImportedLeases(first, [&](auto callback) { first_done = std::move(callback); }, complete));
    ASSERT_TRUE(VideoCore::RetireImportedLeases(last, [&](auto callback) { last_done = std::move(callback); }, complete));
    plan.reset();
    first_done();
    EXPECT_EQ(released, 0);
    last_done();
    EXPECT_EQ(released, 1);
}

TEST(WritebackPlan, RendererRetirementRetainsOwnersOnFailureAndUntilActualTicks) {
    int freed = 0;
    auto owner = std::make_shared<VideoCore::ImportedViewLease>([&] { ++freed; });
    std::vector<std::shared_ptr<void>> stream;
    stream.reserve(2);
    stream.push_back(owner);
    const auto capacity = stream.capacity();
    Native::RetiredWritebackPlans retired;
    EXPECT_FALSE(retired.Retire(stream, 7, [](auto&, auto) { throw 1; }));
    ASSERT_EQ(stream.size(), 1u);
    EXPECT_EQ(freed, 0);
    ASSERT_TRUE(retired.Retire(stream, 7));
    EXPECT_TRUE(stream.empty());
    EXPECT_EQ(stream.capacity(), capacity);
    stream.push_back(owner);
    ASSERT_TRUE(retired.Retire(stream, 11));
    owner.reset();
    retired.Collect(7);
    EXPECT_EQ(freed, 0);
    retired.Collect(10);
    EXPECT_EQ(freed, 0);
    retired.Collect(11);
    EXPECT_EQ(freed, 1);
}

TEST(WritebackPlan, AliasedRangesShareOnePageButDifferentRegionsRemainDistinct) {
    struct Part { std::uint64_t region_begin, page; };
    const std::vector<Part> planned{{0x1000, 0x2000}};
    EXPECT_TRUE(Native::WritebackPageAlreadyPlanned(planned, std::uint64_t{0x1000}, std::uint64_t{0x2000}));
    EXPECT_FALSE(Native::WritebackPageAlreadyPlanned(planned, std::uint64_t{0x1000}, std::uint64_t{0x3000}));
    EXPECT_FALSE(Native::WritebackPageAlreadyPlanned(planned, std::uint64_t{0x4000}, std::uint64_t{0x2000}));
}

TEST(WritebackPlan, OversizedOrUnsupportedWritesForgetRecencyWithoutRejectingOperation) {
    constexpr std::uint64_t limit = 4 * 1024 * 1024;
    EXPECT_FALSE(Native::RegionWritebackNeedsFallback(limit, true, true, true, true, true));
    EXPECT_TRUE(Native::RegionWritebackNeedsFallback(limit + 1, true, true, true, true, true));
    EXPECT_TRUE(Native::RegionWritebackNeedsFallback(4096, true, true, true, false, true));
    EXPECT_TRUE(Native::RegionWritebackNeedsFallback(4096, false, false, false, false, false));
    std::map<int, int> recent{{1, 7}, {2, 8}};
    std::vector<int> forgotten{1};
    Native::ForgetWritebackPages(forgotten, [&](int page) { recent.erase(page); });
    EXPECT_TRUE(forgotten.empty());
    EXPECT_FALSE(recent.contains(1));
    EXPECT_TRUE(recent.contains(2));
}

TEST(StableRegionBindings, RebuildsAfterARegionIsReplaced) {
    unsigned generation = 0, binds = 0, resets = 0;
    const bool ok = Vulkan::BindRegionsStable([&] { return generation; }, [&] {
        if (++binds == 1) ++generation;
        return true;
    }, [&] { ++resets; });
    EXPECT_TRUE(ok);
    EXPECT_EQ(binds, 2u);
    EXPECT_EQ(resets, 1u);
}

TEST(StableRegionBindings, RefusedReadsPreserveRegionsAndWritesInvalidateThem) {
    using Native::RegionMergeDisposition;
    using Native::RegionMergePolicy;
    EXPECT_EQ(RegionMergePolicy(true, false, 4, 16), RegionMergeDisposition::Replace);
    EXPECT_EQ(RegionMergePolicy(false, false, 4, 16), RegionMergeDisposition::Preserve);
    EXPECT_EQ(RegionMergePolicy(false, true, 4, 16), RegionMergeDisposition::Preserve);
    EXPECT_EQ(RegionMergePolicy(false, true, 17, 16), RegionMergeDisposition::Preserve);
}

TEST(StableRegionBindings, DownloadRangesMergeWithoutCoveringUnwrittenBytes) {
    std::vector<Native::PageDownloadRange> ranges;
    Native::AddPageDownloadRange(ranges, 12, 20);
    Native::AddPageDownloadRange(ranges, 4, 8);
    Native::AddPageDownloadRange(ranges, 8, 14);
    Native::AddPageDownloadRange(ranges, 30, 40);
    ASSERT_EQ(ranges.size(), 2u);
    EXPECT_EQ(ranges[0], (Native::PageDownloadRange{4, 20}));
    EXPECT_EQ(ranges[1], (Native::PageDownloadRange{30, 40}));
}

TEST(StableRegionBindings, RegionReplacementIsTransactionalOnAdmissionAndAllocationFailure) {
    struct Region { std::uint64_t begin, end; int contents; };
    std::map<std::uint64_t, Region> regions{{0, {0, 8, 7}}, {8, {8, 16, 9}}};
    std::vector<Region> retired;
    auto remove = [&](Region&& r) { retired.push_back(r); };
    auto try_replace = [&](bool valid, bool write, bool allocated) {
        std::uint64_t begin = 4, end = 12;
        return Native::ReplaceOverlappingRegions(regions, begin, end, write, 8, 16,
            [&](auto, auto) { return valid; }, [&](auto, auto) { return allocated; }, remove);
    };
    EXPECT_EQ(try_replace(false, false, true), Native::RegionMergeDisposition::Preserve);
    ASSERT_EQ(regions.size(), 2u);
    EXPECT_EQ(try_replace(true, true, false), Native::RegionMergeDisposition::Preserve);
    ASSERT_EQ(regions.size(), 2u);
    EXPECT_EQ(regions.at(0).contents, 7);
    EXPECT_EQ(regions.at(8).contents, 9);
    EXPECT_TRUE(retired.empty());
    EXPECT_EQ(try_replace(false, true, true), Native::RegionMergeDisposition::Preserve);
    EXPECT_EQ(regions.size(), 2u);
    EXPECT_TRUE(retired.empty());
    std::uint64_t begin = 4, end = 12;
    EXPECT_THROW(Native::ReplaceOverlappingRegions(regions, begin, end, true, 8, 16,
        [](auto, auto) { return true; }, [](auto, auto) -> bool { throw 1; }, remove), int);
    EXPECT_EQ(regions.size(), 2u);
}

TEST(StableRegionBindings, SnapshotPoolWaitsForItsSubmissionAndBoundsRetainedBytes) {
    struct Buffer { std::uint64_t bytes; std::uint64_t Size() const { return bytes; } };
    Native::RetiredSnapshotPool<Buffer> pool;
    auto first = std::make_unique<Buffer>(Buffer{65536});
    ASSERT_TRUE(pool.Retire(first, 42));
    EXPECT_FALSE(first);
    EXPECT_FALSE(pool.Take(65536, 41));
    auto completed = pool.Take(65536, 42);
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed->Size(), 65536u);
    auto huge = std::make_unique<Buffer>(Buffer{257ull * 1024 * 1024});
    EXPECT_FALSE(pool.Retire(huge, 43));
    EXPECT_TRUE(huge);
    auto later = std::make_unique<Buffer>(Buffer{131072});
    auto earlier = std::make_unique<Buffer>(Buffer{131072});
    ASSERT_TRUE(pool.Retire(later, 100));
    ASSERT_TRUE(pool.Retire(earlier, 50));
    EXPECT_TRUE(pool.Take(131072, 50));
    EXPECT_FALSE(pool.Take(131072, 99));
    EXPECT_TRUE(pool.Take(131072, 100));
}

TEST(StableRegionBindings, FailedSnapshotKeepsThePageWatched) {
    Vulkan::PageWatchTracker watches;
    bool protected_page = false;
    watches.Track(0x1000, 0x2000, [&](auto, auto) { protected_page = true; });
    EXPECT_FALSE(watches.CaptureAndUntrack(0x1000, [&] {
        EXPECT_TRUE(protected_page);
        return false;
    }, [&](auto, auto) { protected_page = false; }));
    EXPECT_TRUE(protected_page);
    EXPECT_TRUE(watches.CaptureAndUntrack(0x1000, [&] {
        EXPECT_TRUE(protected_page);
        return true;
    }, [&](auto, auto) { protected_page = false; }));
    EXPECT_FALSE(protected_page);
    EXPECT_FALSE(watches.CaptureAndUntrack(0x1000, [] { return true; }, [](auto, auto) {}));
}

TEST(StableRegionBindings, ImageExclusionRearmsAndPreventsAheadOpening) {
    Vulkan::PageWatchTracker watches;
    bool protected_page = false;
    watches.Track(0x1000, 0x2000, [&](auto, auto) { protected_page = true; });
    ASSERT_TRUE(watches.CaptureAndUntrack(0x1000, [] { return true; },
                                          [&](auto, auto) { protected_page = false; }));
    ASSERT_FALSE(protected_page);
    watches.ExcludeAndTrack(0x1000, 0x2000, [&](auto, auto) { protected_page = true; });
    ASSERT_TRUE(protected_page);
    EXPECT_FALSE(watches.CaptureAndUntrack(0x1000, [] { return true; },
                                           [&](auto, auto) { protected_page = false; }));
    watches.ClearExclusions(0x1000, 0x2000);
    EXPECT_TRUE(watches.CaptureAndUntrack(0x1000, [] { return true; },
                                          [&](auto, auto) { protected_page = false; }));
}

TEST(StableRegionBindings, ImagePublicationWaitsForAndRearmsAnInProgressCapture) {
    Vulkan::PageWatchTracker watches;
    std::atomic<bool> protected_page{false};
    watches.Track(0x1000, 0x2000, [&](auto, auto) { protected_page = true; });
    std::promise<void> entered, release;
    auto go = release.get_future().share();
    std::thread capture([&] {
        watches.CaptureAndUntrack(0x1000, [&] {
            entered.set_value();
            go.wait();
            return true;
        }, [&](auto, auto) { protected_page = false; });
    });
    entered.get_future().wait();
    std::thread publish([&] {
        watches.ExcludeAndTrack(0x1000, 0x2000, [&](auto, auto) { protected_page = true; });
    });
    release.set_value();
    capture.join();
    publish.join();
    EXPECT_TRUE(protected_page);
    EXPECT_FALSE(watches.CaptureAndUntrack(0x1000, [] { return true; }, [](auto, auto) {}));
}

#ifdef _WIN32
TEST(StableRegionBindings, PageSnapshotFailsWithoutFaultOnUnreadableMemory) {
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(page, nullptr);
    static_cast<unsigned char*>(page)[0] = 42;
    unsigned char copy[4096]{};
    DWORD old_protection = 0;
    ASSERT_TRUE(VirtualProtect(page, 4096, PAGE_READONLY, &old_protection));
    EXPECT_TRUE(Native::CopyPageWithoutFault(copy, page, sizeof(copy)));
    EXPECT_EQ(copy[0], 42);
    ASSERT_TRUE(VirtualProtect(page, 4096, PAGE_NOACCESS, &old_protection));
    EXPECT_FALSE(Native::CopyPageWithoutFault(copy, page, sizeof(copy)));
    EXPECT_TRUE(VirtualFree(page, 0, MEM_RELEASE));
}
#endif

TEST(StableRegionBindings, RejectsUnstableOrFailedBindings) {
    unsigned generation = 0, resets = 0;
    EXPECT_FALSE(Vulkan::BindRegionsStable([&] { return generation; }, [&] {
        ++generation;
        return true;
    }, [&] { ++resets; }));
    EXPECT_EQ(resets, 4u);
    EXPECT_FALSE(Vulkan::BindRegionsStable([&] { return generation; }, [] { return false; },
                                          [&] { ++resets; }));
    EXPECT_EQ(resets, 4u);
}
