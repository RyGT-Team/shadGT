// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <array>
#include <unordered_map>
#include "ryty_gpu/native/linear_image_download.h"

TEST(LinearImageDownload, SplitsPartsAndPreservesPaddingAcrossMipsAndLayers) {
    using namespace Native;
    const std::array parts{LinearDownloadPart{0,8,0,8}, LinearDownloadPart{8,32,4,36},
                           LinearDownloadPart{40,120,8,128}};
    const std::array mips{LinearDownloadMip{0,6,3,4,3,1}, LinearDownloadMip{80,4,2,2,2,2}};
    std::vector<LinearDownloadCopy> copies;
    ASSERT_TRUE(PlanLinearImageDownload(mips, parts, 160, 4, copies));
    EXPECT_EQ(copies[0].width, 2u);
    EXPECT_EQ(copies[1].x, 2u);
    std::array<unsigned char, 160> result{}; result.fill(0x5a);
    for (const auto& c : copies) for (std::uint32_t y=0; y<c.height; ++y) {
        const auto offset = c.range_offset + std::uint64_t(y) * c.pitch * 4;
        std::fill_n(result.begin() + offset, c.width * 4, 0x77);
    }
    EXPECT_EQ(result[0], 0x77); EXPECT_EQ(result[15], 0x77); EXPECT_EQ(result[16], 0x5a);
    EXPECT_EQ(result[24], 0x77); EXPECT_EQ(result[40], 0x5a);
    EXPECT_EQ(result[80], 0x77); EXPECT_EQ(result[88], 0x5a);
    EXPECT_EQ(result[128], 0x77); EXPECT_EQ(result[136], 0x5a);
}

TEST(LinearImageDownload, CoalescesWholeRowsAndRejectsInvalidCoverage) {
    using namespace Native;
    std::vector<LinearDownloadCopy> copies;
    const std::array mips{LinearDownloadMip{0,8,4,8,4,1}};
    const std::array contiguous{LinearDownloadPart{0,128,0,128}};
    ASSERT_TRUE(PlanLinearImageDownload(mips, contiguous, 128, 4, copies));
    ASSERT_EQ(copies.size(), 1u);
    EXPECT_EQ(copies[0].height, 4u);
    const std::array gap{LinearDownloadPart{0,4,0,4}, LinearDownloadPart{8,120,0,120}};
    EXPECT_FALSE(PlanLinearImageDownload(mips, gap, 128, 4, copies));
    const std::array phase{LinearDownloadPart{0,6,0,6}, LinearDownloadPart{6,122,0,122}};
    EXPECT_FALSE(PlanLinearImageDownload(mips, phase, 128, 4, copies));
    const std::array bad_view{LinearDownloadPart{0,128,4,128}};
    EXPECT_FALSE(PlanLinearImageDownload(mips, bad_view, 128, 4, copies));
    const std::array overflow{LinearDownloadMip{~std::uint64_t{0} - 4,8,4,8,4,1}};
    EXPECT_FALSE(PlanLinearImageDownload(overflow, contiguous, 128, 4, copies));
    const std::array overlap{LinearDownloadMip{0,8,2,8,2,1}, LinearDownloadMip{32,4,2,4,2,1}};
    EXPECT_FALSE(PlanLinearImageDownload(overlap, contiguous, 128, 4, copies));
}

TEST(LinearImageDownload, PlansAbove256MiBWithoutImageSizedScratch) {
    using namespace Native;
    constexpr std::uint64_t total = 8192ull * 33000;
    constexpr std::uint64_t split = (128ull << 20) + 16;
    const std::array parts{LinearDownloadPart{0,split,0,split},
                           LinearDownloadPart{split,total-split,0,total-split}};
    const std::array mips{LinearDownloadMip{0,2048,33000,2047,33000,1}};
    std::vector<LinearDownloadCopy> copies;
    ASSERT_TRUE(PlanLinearImageDownload(mips, parts, total, 4, copies));
    EXPECT_LT(copies.size(), 10u); // only a few coalesced row runs and boundary fragments
}

TEST(LinearImageDownload, OnlyOverlappingMappingChangesCancel) {
    EXPECT_TRUE(Native::LinearDownloadOverlaps(0x1000, 0x100, 0x1080, 0x20));
    EXPECT_TRUE(Native::LinearDownloadOverlaps(0x1000, 0x100, 0x0ff0, 0x20));
    EXPECT_FALSE(Native::LinearDownloadOverlaps(0x1000, 0x100, 0x1100, 0x20));
    EXPECT_FALSE(Native::LinearDownloadOverlaps(0x1000, 0x100, 0x8000, 0x20));
    EXPECT_FALSE(Native::LinearDownloadOverlaps(0x1000, 0, 0x1000, 0x20));
}

TEST(LinearImageDownload, CanceledQueueEntryCanBeAdmittedAgain) {
    int first = 1, other = 2;
    std::unordered_map<const int*, int> plans{{&first, 1}, {&other, 2}};
    std::vector<int*> queue{&first, &other};
    Native::CancelOverlappingLinearPlans(plans, queue, [&](const int* key) { return key == &first; });
    ASSERT_EQ(queue.size(), 1u);
    EXPECT_EQ(queue.front(), &other);
    EXPECT_EQ(plans.count(&first), 0u);
    plans.emplace(&first, 3);
    queue.push_back(&first);
    EXPECT_EQ(queue.size(), 2u);
    EXPECT_EQ(plans.at(&first), 3);
}

TEST(LinearImageDownload, RequiresExactImageShape) {
    using namespace Native;
    const LinearImageShape guest{512, 257, 1, 4, 6, 1};
    EXPECT_TRUE(LinearImageShapeMatches(guest, guest, true));
    auto actual = guest; actual.mips = 3;
    EXPECT_FALSE(LinearImageShapeMatches(guest, actual, true));
    actual = guest; actual.layers = 5;
    EXPECT_FALSE(LinearImageShapeMatches(guest, actual, true));
    actual = guest; actual.width++;
    EXPECT_FALSE(LinearImageShapeMatches(guest, actual, true));
    EXPECT_FALSE(LinearImageShapeMatches(guest, guest, false));
}

TEST(LinearImageDownload, MetadataFailureAfterRecordRetainsLeases) {
    struct Part { std::shared_ptr<void> lease; };
    auto owner = std::make_shared<int>(17);
    std::weak_ptr<int> weak = owner;
    std::vector<Part> parts{{owner}};
    std::vector<std::shared_ptr<void>> stream;
    stream.reserve(parts.size());
    owner.reset();
    EXPECT_THROW({
        Native::TransferLinearDownloadLeases(stream, parts);
        throw std::bad_alloc{}; // metadata allocation after command recording
    }, std::bad_alloc);
    EXPECT_FALSE(weak.expired());
    EXPECT_FALSE(parts.front().lease);
    stream.clear();
    EXPECT_TRUE(weak.expired());
}
