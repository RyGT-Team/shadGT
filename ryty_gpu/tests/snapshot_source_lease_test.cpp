// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/snapshot_source_lease.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace Vulkan::SnapshotLease;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); } } while (false)

struct MockPoison {
    std::vector<U> granules;
    bool Safe(Range r) const {
        U end{}; if (!End(r, end)) return false;
        for (auto g : granules) if (g >= r.address / Granule && g <= (end - 1) / Granule) return false;
        return true;
    }
    void MarkUnsafe(Range r) {
        U end{}; CHECK(End(r, end));
        for (U g = r.address / Granule; g <= (end - 1) / Granule; ++g) granules.push_back(g);
    }
};

int main() {
    const Mapping proof{7, true, true, true}, current{7, true, true, false};
    MockPoison poison; StateTable table{poison}; ListTokens list;
    Range source{0x1000000, 4096};
    CHECK(!table.KnownBacking(source, current));
    auto id = table.Begin(source, proof, 1); CHECK(id);
    CHECK(!table.KnownBacking(source, current));
    CHECK(table.Commit(*id, proof, proof, true, true));
    auto receipt = table.KnownBacking(source, current); CHECK(receipt && Good(*receipt));
    CHECK(ReuseBackingReceipt(source, source, *receipt, current, true));
    CHECK(!ReuseBackingReceipt(source, {source.address, source.size - 1}, *receipt, current, true));
    CHECK(!ReuseBackingReceipt(source, source, *receipt, {8, true, true, false}, true));
    CHECK(!ReuseBackingReceipt(source, source, *receipt, current, false));
    CHECK(!table.KnownBacking(source, {7, false, true, false}));
    auto token = table.Bind(*id, list.Epoch(), source, proof, proof, true, true); CHECK(token.valid);
    CHECK(table.ReuseChecked(token, list.Epoch(), source, true, [&] { return proof; }, 200));
    CHECK(table.ReleaseUnused(122, 120).count == 0); // hot owner timestamp wins
    CHECK(table.ReleaseUnused(100, 0).count == 0);   // backwards clock
    CHECK(table.ReleaseUnused(0, 0).count == 0);
    CHECK(table.ReleaseUnused(321, 120).count == 1);
    CHECK(!table.Reuse(token, list.Epoch(), source, proof, true));
    CHECK(!table.KnownBacking(source, current));

    auto cold = table.Begin(source, proof, 1); CHECK(cold && table.Commit(*cold, proof, proof, true, true));
    CHECK(cold->slot == id->slot && cold->generation != id->generation);
    CHECK(!table.Reuse(token, list.Epoch(), source, proof, true)); // same slot, range and epoch
    poison.MarkUnsafe({source.address, 1});
    CHECK(!table.KnownBacking(source, current));
    CHECK(table.ReleaseUnsafe(false).count == 1);

    MockPoison capacity_poison; StateTable capacity{capacity_poison}; std::array<Id, 64> ids{};
    for (unsigned i = 0; i < ids.size(); ++i) {
        Range r{0x10000000 + U(i) * Granule, 4096}; CHECK(capacity.HasCapacityFor(r));
        auto next = capacity.Begin(r, proof, 1); CHECK(next); ids[i] = *next;
        CHECK(capacity.Commit(*next, proof, proof, true, true));
    }
    Range new_source{0x20000000, 4096}; CHECK(!capacity.HasCapacityFor(new_source));
    CHECK(capacity.Release(ids[0], false)); CHECK(capacity.HasCapacityFor(new_source));
    CHECK(capacity.Begin(new_source, proof, 1));

    MockPoison budget_poison; StateTable budget{budget_poison};
    CHECK(!budget.HasCapacityFor({0x30000001, Budget}));
    auto full = budget.Begin({0x30000000, Budget}, proof, 1); CHECK(full);
    CHECK(!budget.HasCapacityFor(new_source)); CHECK(budget.Commit(*full, proof, proof, true, true));
    CHECK(budget.ReleaseUnused(122, 120).count == 1); CHECK(budget.HasCapacityFor(new_source));
    std::puts("PASS snapshot source lease");
}
