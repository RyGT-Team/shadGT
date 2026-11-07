// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <span>
#include "video_core/renderer_vulkan/immutable_upload_memo.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "CHECK failed at %d: %s\n", __LINE__, #x); std::abort(); } } while (false)
using Memo = Vulkan::ImmutableUploadMemo<unsigned, 2, 4, 64>;
static auto collision = [](const void*, std::size_t) { return std::uint64_t{0}; };
int main() {
    Memo memo;
    std::array<std::uint8_t, 32> bytes{};
    auto lookup = memo.Find(0, bytes, collision);
    CHECK(!lookup.binding && memo.Store(lookup, 0, bytes, 17));
    auto check = [&](std::span<const std::uint8_t> candidate) {
        return memo.WithLast(0, candidate.size(), [&](auto saved) {
            return std::equal(saved.begin(), saved.end(), candidate.begin(), candidate.end());
        });
    };
    CHECK(check(bytes) == 17);
    for (auto index : {0u, 16u, 31u}) {
        auto modified = bytes;
        modified[index] = 1;
        CHECK(!check(modified));
        CHECK(!memo.Find(0, modified, collision).binding); // forced hash collision
    }
    bool called = false;
    CHECK(!memo.WithLast(1, bytes.size(), [&](auto) { called = true; return true; }));
    CHECK(!memo.WithLast(0, bytes.size() - 1, [&](auto) { called = true; return true; }));
    CHECK(!memo.WithLast(2, bytes.size(), [&](auto) { called = true; return true; }));
    CHECK(!called);
    CHECK(!memo.WithLast(0, bytes.size(), [](auto) { return false; }));
    CHECK(check(bytes) == 17);

    std::array<std::uint8_t, 65> oversize{};
    auto bad = memo.Find(0, oversize, collision);
    CHECK(!memo.Store(bad, 0, oversize, 99));
    CHECK(check(bytes) == 17); // failed store must not publish a binding
    for (unsigned i = 1; i <= 8; ++i) {
        auto other = bytes; other[0] = static_cast<std::uint8_t>(i);
        auto ticket = memo.Find(1, other, collision);
        CHECK(!ticket.binding && memo.Store(ticket, 1, other, 100 + i));
    }
    CHECK(!check(bytes)); // last stage-zero slot was replaced by another stage
    memo.Clear();
    CHECK(!memo.WithLast(1, bytes.size(), [](auto) { return true; }));
    CHECK(!memo.Find(0, bytes, collision).binding);
    std::puts("PASS: exact last bytes, collision, eviction, rejected store, list clear");
}
