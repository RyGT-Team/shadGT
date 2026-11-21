// SPDX-License-Identifier: GPL-2.0-or-later
// FIX-SGT-044: the kept descriptor sets' per-handle lists (Native::KeptSetUsers).
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "ryty_gpu/native/kept_set_table.h"
#include "ryty_gpu/native/kept_set_users.h"

namespace {
using Native::KeptSetUsers;
using Key = std::pair<std::uint64_t, std::uint64_t>;

Key KeyOf(std::uint64_t n) {
    // spread like the XXH3 halves the renderer uses
    std::uint64_t a = n * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
    a ^= a >> 29;
    return {a, ~n};
}

std::vector<Key> DropAll(KeptSetUsers& users, std::uint64_t handle) {
    std::vector<Key> erased;
    users.Drop(handle, [&](std::uint64_t h, std::uint64_t c) { erased.emplace_back(h, c); });
    return erased;
}

TEST(KeptSetUsers, AHandleBoundSeveralTimesGoesInOnce) {
    KeptSetUsers users;
    const std::uint64_t layout = 7, buffer = 100, view = 200, sampler = 300, arena = 999;
    const std::vector<std::uint64_t> handles{layout, buffer, buffer, arena, view, sampler,
                                             view,   sampler, 0,     buffer};
    const Key k = KeyOf(1);
    users.Add(k.first, k.second, handles, arena);
    EXPECT_EQ(users.ListSize(layout), 1u);
    EXPECT_EQ(users.ListSize(buffer), 1u);
    EXPECT_EQ(users.ListSize(view), 1u);
    EXPECT_EQ(users.ListSize(sampler), 1u);
    EXPECT_EQ(users.ListSize(arena), 0u);
    EXPECT_EQ(users.ListSize(0), 0u);
    EXPECT_EQ(users.Kept(), 1u);
    EXPECT_EQ(DropAll(users, sampler), std::vector<Key>{k});
    EXPECT_EQ(users.Kept(), 0u);
}

TEST(KeptSetUsers, DropErasesEachSetOnceAndForgetsTheHandle) {
    KeptSetUsers users;
    const std::uint64_t shared = 50;
    std::vector<Key> keys;
    for (std::uint64_t n = 0; n < 3; ++n) {
        keys.push_back(KeyOf(n));
        const std::vector<std::uint64_t> handles{shared, 1000 + n, shared};
        users.Add(keys.back().first, keys.back().second, handles, 0);
    }
    auto erased = DropAll(users, shared);
    std::sort(erased.begin(), erased.end());
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(erased, keys);
    EXPECT_EQ(users.Kept(), 0u);
    EXPECT_FALSE(users.Drop(shared, [](std::uint64_t, std::uint64_t) { FAIL(); }));
    // the sets are gone from the other handles' lists too
    for (std::uint64_t n = 0; n < 3; ++n) {
        EXPECT_TRUE(DropAll(users, 1000 + n).empty());
    }
}

TEST(KeptSetUsers, ASetsOtherListsGoStaleWhenItIsDropped) {
    KeptSetUsers users;
    const Key a = KeyOf(1), b = KeyOf(2);
    users.Add(a.first, a.second, std::vector<std::uint64_t>{10, 20}, 0);
    users.Add(b.first, b.second, std::vector<std::uint64_t>{20, 30}, 0);
    EXPECT_EQ(DropAll(users, 10), std::vector<Key>{a});
    EXPECT_EQ(DropAll(users, 20), std::vector<Key>{b}); // a is not erased a second time
    EXPECT_TRUE(DropAll(users, 30).empty());
}

// What happened in the GT7 race of 20-11: a handle every set holds, bound more than twice in
// each, and sets of the same key stored again after a drop. With FIX-SGT-044 neither makes a
// pass that removes nothing.
TEST(KeptSetUsers, ManyBindingsOfOneHandleNeverMakeAPass) {
    KeptSetUsers users;
    const std::uint64_t sampler = 77;
    for (std::uint64_t n = 0; n < 100000; ++n) {
        const Key k = KeyOf(n);
        users.Add(k.first, k.second, std::vector<std::uint64_t>{5, sampler, sampler, sampler, n + 1000},
                  0);
    }
    const auto stats = users.TakeStats();
    EXPECT_EQ(stats.passes, 0u);
    EXPECT_EQ(users.ListSize(sampler), 100000u);
}

TEST(KeptSetUsers, DropAndStoreAgainKeepsListsShortAndPassesCheap) {
    KeptSetUsers users;
    const std::uint64_t shared = 77;
    constexpr std::uint64_t Sets = 20000;
    for (std::uint64_t n = 0; n < Sets; ++n) {
        const Key k = KeyOf(n);
        users.Add(k.first, k.second, std::vector<std::uint64_t>{shared, 1000000 + n}, 0);
    }
    // Each round, one set's view dies and an equal set (same key, same handles) is stored again.
    std::uint64_t adds = Sets;
    std::mt19937_64 random{1};
    for (int round = 0; round < 1000000; ++round) {
        const std::uint64_t n = random() % Sets;
        const auto erased = DropAll(users, 1000000 + n);
        ASSERT_EQ(erased.size(), 1u);
        users.Add(erased[0].first, erased[0].second,
                  std::vector<std::uint64_t>{shared, 1000000 + n}, 0);
        ++adds;
        ASSERT_LE(users.ListSize(shared), 2 * users.Kept() + 4097);
    }
    const auto stats = users.TakeStats();
    EXPECT_GT(stats.passes, 0u);
    EXPECT_LE(stats.scanned, 2 * adds) << "a pass went over entries it did not remove";
    EXPECT_GT(stats.removed * 2, stats.scanned) << "each pass removes more than half its list";
    EXPECT_EQ(DropAll(users, shared).size(), Sets);
}

TEST(KeptSetUsers, MassDropThenRefill) {
    KeptSetUsers users;
    const std::uint64_t shared = 77;
    std::uint64_t serial = 0, adds = 0;
    for (int wave = 0; wave < 5; ++wave) {
        std::vector<std::uint64_t> views;
        for (int n = 0; n < 50000; ++n) {
            const Key k = KeyOf(serial);
            views.push_back(5000000 + serial);
            users.Add(k.first, k.second, std::vector<std::uint64_t>{shared, views.back()}, 0);
            ++serial;
            ++adds;
            ASSERT_LE(users.ListSize(shared), 2 * users.Kept() + 4097);
        }
        for (const auto view : views) ASSERT_EQ(DropAll(users, view).size(), 1u);
        EXPECT_EQ(users.Kept(), 0u);
    }
    const auto stats = users.TakeStats();
    EXPECT_LE(stats.scanned, 2 * adds);
    EXPECT_TRUE(DropAll(users, shared).empty());
}

// Random adds and drops against a plain model: a Drop erases exactly the kept sets that hold the
// handle, once each, and no kept set is ever lost from a list.
TEST(KeptSetUsers, MatchesAModelUnderRandomUse) {
    KeptSetUsers users;
    std::map<Key, std::set<std::uint64_t>> model; // kept set -> its handles
    std::mt19937_64 random{42};
    std::uint64_t next_key = 0;
    for (int step = 0; step < 200000; ++step) {
        if (random() % 4 != 0 || model.empty()) {
            std::vector<std::uint64_t> handles;
            const int count = 1 + int(random() % 6);
            for (int i = 0; i < count; ++i) handles.push_back(1 + random() % 300);
            handles.push_back(handles.front()); // a repeat
            // sometimes the key of a set dropped before (stored again)
            const Key k = KeyOf(random() % 8 == 0 && next_key ? random() % next_key : next_key++);
            if (model.contains(k)) continue; // the renderer stores a key only on a miss
            users.Add(k.first, k.second, handles, 0);
            model[k] = std::set<std::uint64_t>(handles.begin(), handles.end());
        } else {
            const std::uint64_t handle = 1 + random() % 300;
            auto erased = DropAll(users, handle);
            std::sort(erased.begin(), erased.end());
            std::vector<Key> expected;
            for (auto it = model.begin(); it != model.end();) {
                if (it->second.contains(handle)) {
                    expected.push_back(it->first);
                    it = model.erase(it);
                } else {
                    ++it;
                }
            }
            ASSERT_EQ(erased, expected) << "step " << step << " handle " << handle;
        }
        ASSERT_EQ(users.Kept(), model.size());
    }
}

// The rule PERF-GTS-378 had, on the same use as above (a sampler bound three times in each set):
// the passes find every copy in the table, remove nothing, and run again at each store.
TEST(KeptSetUsers, OldRuleScannedTheWholeListAtEveryStore) {
    Native::KeptSetTable<std::uint64_t> table;
    std::map<std::uint64_t, std::vector<Key>> lists;
    std::uint64_t looked_up = 0, passes = 0;
    const std::uint64_t sampler = 77;
    constexpr std::uint64_t Sets = 10000;
    for (std::uint64_t n = 0; n < Sets; ++n) {
        const Key k = KeyOf(n);
        table.Insert(k.first, k.second, 1, n + 1);
        for (const std::uint64_t handle : {std::uint64_t{5}, sampler, sampler, sampler}) {
            auto& list = lists[handle];
            if (list.size() > 2 * table.Size() + 4096) {
                ++passes;
                looked_up += list.size();
                std::erase_if(list, [&](const Key& key) { return !table.Find(key.first, key.second); });
            }
            list.emplace_back(k);
        }
    }
    // From about 4,100 sets on, every store makes three passes over the whole sampler list.
    EXPECT_GT(passes, 3 * (Sets - 4200));
    EXPECT_GT(looked_up, std::uint64_t{300} * 1000 * 1000);
    std::printf("old rule, %llu sets: %llu passes, %llu lookups (%.0f a store)\n",
                (unsigned long long)Sets, (unsigned long long)passes,
                (unsigned long long)looked_up, double(looked_up) / double(Sets));
}

// The longest pass the new rule can make at the table's flush limit (200,000 kept sets), timed.
TEST(KeptSetUsers, LongestPassAtTheFlushLimitIsShort) {
    KeptSetUsers users;
    const std::uint64_t shared = 77;
    constexpr std::uint64_t Kept = 200000;
    std::uint64_t key = 0;
    for (std::uint64_t n = 0; n < Kept; ++n, ++key) {
        const Key k = KeyOf(key);
        users.Add(k.first, k.second, std::vector<std::uint64_t>{shared, 10000000 + key}, 0);
    }
    // churn until the shared list is one entry short of its pass
    while (users.ListSize(shared) <= 2 * users.Kept() + 4096) {
        const std::uint64_t victim = 10000000 + key - Kept;
        ASSERT_EQ(DropAll(users, victim).size(), 1u);
        const Key k = KeyOf(key);
        users.Add(k.first, k.second, std::vector<std::uint64_t>{shared, 10000000 + key}, 0);
        ++key;
    }
    users.TakeStats();
    ASSERT_EQ(DropAll(users, 10000000 + key - Kept).size(), 1u); // the table stays at its limit
    const Key k = KeyOf(key);
    const auto start = std::chrono::steady_clock::now();
    users.Add(k.first, k.second, std::vector<std::uint64_t>{shared, 10000000 + key}, 0);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const auto stats = users.TakeStats();
    EXPECT_EQ(stats.passes, 1u);
    EXPECT_GT(stats.scanned, 2 * Kept);
    std::printf("longest pass: %llu entries in %.2f ms\n", (unsigned long long)stats.scanned, ms);
    EXPECT_LT(ms, 25.0); // about 1 ms on a Ryzen 7 8700G; loose for slower CPUs and debug builds
}
} // namespace
