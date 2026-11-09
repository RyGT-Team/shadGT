// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <chrono>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/shader_store_readers.h"

namespace {
struct Listing {
    struct Entry { bool has_spv; size_t perm_idx; };
    std::vector<Entry> entries;
    u64 hash{};
};
}

TEST(ShaderStoreReaders, ListingIsReadyBeforePrefetchAndShutdownJoins) {
    using namespace std::chrono_literals;
    std::binary_semaphore entered{0}, release{0}, stopped{0};
    auto readers=std::make_unique<Vulkan::ShaderStoreReaders<Listing>>(1,
        [](u64 hash) { return Listing{{{true,3}},hash}; },
        [&](u64,size_t) { entered.release(); release.acquire(); });
    auto result=readers->List(42);
    const bool entered_ok=entered.try_acquire_for(2s);
    EXPECT_TRUE(entered_ok);
    EXPECT_EQ(result.wait_for(0s),std::future_status::ready);
    if (entered_ok) EXPECT_EQ(result.get().hash,42u);
    std::jthread shutdown([&] { readers.reset(); stopped.release(); });
    if (entered_ok) EXPECT_FALSE(stopped.try_acquire_for(100ms));
    release.release();
    EXPECT_TRUE(stopped.try_acquire_for(2s));
}

TEST(ShaderStoreReaders, ReadFailureDoesNotStopNextJob) {
    Vulkan::ShaderStoreReaders<Listing> readers(1,
        [](u64 hash) -> Listing { if (hash==1) throw 7; return {{},hash}; },
        [](u64,size_t) {});
    auto failed=readers.List(1);
    auto next=readers.List(2);
    EXPECT_THROW(failed.get(),int);
    EXPECT_EQ(next.get().hash,2u);
}

TEST(ShaderStoreReaders, PrefetchesAtMostEightExistingBinaries) {
    using namespace std::chrono_literals;
    std::atomic<unsigned> calls{};
    std::binary_semaphore done{0};
    Vulkan::ShaderStoreReaders<Listing> readers(1,
        [](u64 hash) { Listing listing{{},hash}; for(size_t i=0;i<20;++i) listing.entries.push_back({true,i}); return listing; },
        [&](u64,size_t) { if (++calls==8) done.release(); });
    EXPECT_EQ(readers.List(9).get().hash,9u);
    EXPECT_TRUE(done.try_acquire_for(2s));
    EXPECT_EQ(calls.load(),8u);
}
