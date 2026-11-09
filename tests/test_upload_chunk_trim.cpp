// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <memory>
#include <deque>
#include <vector>
#include "video_core/renderer_vulkan/upload_chunk_cache.h"

namespace {
struct Buffer { std::uint64_t size; std::uint64_t Size() const { return size; } };
struct Chunk { std::unique_ptr<Buffer> buffer; std::uint64_t retire_tick{}; };
}

TEST(UploadChunkTrim, RespectsLowWatermarkReserveAndBatchLimit) {
    std::vector<Chunk> free, dropped;
    for (int i = 0; i < 100; ++i) free.push_back({std::make_unique<Buffer>(Buffer{1024}), 0});
    std::size_t low = 80;
    Vulkan::TrimIdleUploadChunks(free, low, dropped);
    EXPECT_EQ(dropped.size(), 32u);
    EXPECT_EQ(free.size(), 68u);
    EXPECT_EQ(low, 68u);
    dropped.clear(); low = 20;
    Vulkan::TrimIdleUploadChunks(free, low, dropped);
    EXPECT_EQ(dropped.size(), 4u);
    EXPECT_EQ(low, 64u);
    dropped.clear(); low = 16;
    Vulkan::TrimIdleUploadChunks(free, low, dropped);
    EXPECT_TRUE(dropped.empty());
}

TEST(UploadChunkTrim, NeverTrimsAnUncompletedRetiredChunk) {
    std::vector<Chunk> free, dropped;
    std::deque<Chunk> retired;
    for (int i = 0; i < 20; ++i) retired.push_back({std::make_unique<Buffer>(Buffer{1024}), 8});
    Vulkan::CacheCompletedUploadChunks(free, retired, 7);
    std::size_t low = 100;
    Vulkan::TrimIdleUploadChunks(free, low, dropped);
    EXPECT_TRUE(dropped.empty());
    EXPECT_EQ(retired.size(), 20u);
    Vulkan::CacheCompletedUploadChunks(free, retired, 8);
    EXPECT_EQ(free.size(), 20u);
    EXPECT_TRUE(retired.empty());
    low = 100; // even a stale high watermark cannot trim below the reserve
    Vulkan::TrimIdleUploadChunks(free, low, dropped);
    EXPECT_EQ(dropped.size(), 4u);
    EXPECT_EQ(free.size(), 16u);
}
