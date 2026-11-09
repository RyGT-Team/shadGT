// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <span>
#include "video_core/renderer_vulkan/immutable_upload_memo.h"
#include "video_core/renderer_vulkan/snapshot_source_hints.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "CHECK failed at %d: %s\n", __LINE__, #x); std::abort(); } } while (false)
using Memo = Vulkan::ImmutableUploadMemo<unsigned, 2, 4, 64>;
static auto collision = [](const void*, std::size_t) { return std::uint64_t{0}; };
int main() {
    Memo memo;
    std::array<std::uint8_t, 32> bytes{};
    auto lookup = memo.Find(0, bytes, collision);
    CHECK(!lookup.binding && memo.Store(lookup, 0, bytes, 17));
    const auto original_slot = lookup.slot;
    auto check = [&](std::span<const std::uint8_t> candidate) {
        return memo.WithLast(0, candidate.size(), [&](auto saved) {
            return std::equal(saved.begin(), saved.end(), candidate.begin(), candidate.end());
        });
    };
    CHECK(check(bytes) == 17);
    auto exact = [&](std::size_t slot, std::span<const std::uint8_t> candidate) {
        return memo.WithSlot(0, slot, candidate.size(), [&](auto saved) {
            return std::equal(saved.begin(), saved.end(), candidate.begin(), candidate.end());
        });
    };
    CHECK(exact(original_slot, bytes) == 17);
    for (auto index : {0u, 16u, 31u}) {
        auto modified = bytes;
        modified[index] = 1;
        CHECK(!check(modified));
        CHECK(!exact(original_slot, modified));
        CHECK(!memo.Find(0, modified, collision).binding); // forced hash collision
    }
    bool called = false;
    CHECK(!memo.WithLast(1, bytes.size(), [&](auto) { called = true; return true; }));
    CHECK(!memo.WithLast(0, bytes.size() - 1, [&](auto) { called = true; return true; }));
    CHECK(!memo.WithLast(2, bytes.size(), [&](auto) { called = true; return true; }));
    CHECK(!called);
    CHECK(!memo.WithSlot(1, original_slot, bytes.size(), [&](auto) { called = true; return true; }));
    CHECK(!memo.WithSlot(0, original_slot, bytes.size() - 1, [&](auto) { called = true; return true; }));
    CHECK(!memo.WithSlot(0, 4, bytes.size(), [&](auto) { called = true; return true; }));
    CHECK(!memo.WithSlot(2, original_slot, bytes.size(), [&](auto) { called = true; return true; }));
    CHECK(!called);
    CHECK(!memo.WithLast(0, bytes.size(), [](auto) { return false; }));
    CHECK(check(bytes) == 17);

    std::array<std::uint8_t, 65> oversize{};
    auto bad = memo.Find(0, oversize, collision);
    CHECK(!memo.Store(bad, 0, oversize, 99));
    CHECK(check(bytes) == 17); // failed store must not publish a binding
    CHECK(exact(original_slot, bytes) == 17);
    auto second = bytes; second[0] = 42;
    auto second_ticket = memo.Find(0, second, collision);
    CHECK(memo.Store(second_ticket, 0, second, 18));
    CHECK(exact(original_slot, bytes) == 17); // explicit non-last candidate
    CHECK(exact(second_ticket.slot, second) == 18);
    for (unsigned i = 1; i <= 8; ++i) {
        auto other = bytes; other[0] = static_cast<std::uint8_t>(i);
        auto ticket = memo.Find(1, other, collision);
        CHECK(!ticket.binding && memo.Store(ticket, 1, other, 100 + i));
    }
    CHECK(!check(bytes)); // last stage-zero slot was replaced by another stage
    CHECK(!exact(original_slot, bytes));
    CHECK(!memo.WithSlot(0, original_slot, bytes.size(), [](auto) { return true; }));
    auto replacement = memo.Find(0, bytes, collision);
    CHECK(memo.Store(replacement, 0, bytes, 33));
    auto replacement_bytes = bytes; replacement_bytes[2] = 77;
    // A previously saved slot is only a candidate: replacing it changes both bytes and binding.
    Memo::Lookup same_slot{}; same_slot.slot = replacement.slot; same_slot.hash = 0;
    CHECK(memo.Store(same_slot, 0, replacement_bytes, 34));
    CHECK(!exact(replacement.slot, bytes));
    CHECK(exact(replacement.slot, replacement_bytes) == 34);
    memo.Clear();
    CHECK(!memo.WithLast(1, bytes.size(), [](auto) { return true; }));
    CHECK(!memo.WithSlot(0, replacement.slot, bytes.size(), [](auto) { return true; }));
    CHECK(!memo.Find(0, bytes, collision).binding);
    Vulkan::SnapshotSourceHints<4> hints;
    CHECK(!hints.Candidate(0x1000, 32));
    hints.Remember(0, 0x1000, 32);
    hints.Remember(1, 0x2000, 32);
    for (int i = 0; i < 20; ++i) {
        CHECK(hints.Candidate(0x1000, 32) == 0);
        CHECK(hints.Candidate(0x2000, 32) == 1);
    }
    CHECK(!hints.Candidate(0x1000, 31));
    hints.Remember(2, 0x1000, 32); // relocation invalidates the prior slot
    CHECK(hints.Candidate(0x1000, 32) == 2);
    hints.Forget(0);
    CHECK(hints.Candidate(0x1000, 32) == 2);
    hints.Remember(2, 0x3000, 64); // overwritten slot no longer identifies A
    CHECK(!hints.Candidate(0x1000, 32));
    CHECK(hints.Candidate(0x3000, 64) == 2);
    hints.Forget(2); // called before a store that may fail
    CHECK(!hints.Candidate(0x3000, 64));
    hints.Remember(4, 0x4000, 32);
    hints.Forget(4);
    CHECK(!hints.Candidate(0x4000, 32));
    CHECK(hints.Candidate(0x2000, 32) == 1);
    hints.Clear();
    CHECK(!hints.Candidate(0x2000, 32));
    std::puts("PASS: exact last bytes, collision, eviction, rejected store, list clear");
}
