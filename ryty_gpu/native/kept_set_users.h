// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Native {
// FIX-SGT-044: which kept descriptor sets (KeptSetTable, PERF-GTS-375) hold each handle, so the
// sets of a handle about to be destroyed can be dropped.
//
// PERF-GTS-378 kept, per handle, the key hashes of the sets stored with it, and thinned a list
// longer than twice the table by looking each of its keys up in the table. A key went into a
// list once per binding of the handle in the set, and again each time an equal set was stored
// after a drop; all those copies were found in the table, so a pass removed almost nothing and
// ran again at the next store of a set holding that handle. In a GT7 race (20-11) the binder
// thread spent its time in such passes, over a list that still had 147,648 entries ahead of one
// and that the passes barely shortened, holding the table's lock: the recorder waited on the
// lock, the command thread on the binder, and the game ran under 1 fps for minutes.
//
// Here each stored set has an id, a list entry is the id with the id's serial when the set was
// stored, and a set goes once into the list of each distinct handle it holds. Dropping a set
// changes its id's serial, so its entries in the other handles' lists are stale. A list holds
// at most one current entry per kept set: a pass, made when a list is longer than twice the
// kept sets plus 4,096, removes more than half of it, and costs at most two checks for each
// entry ever added. A check reads a serial (4 bytes an id), not the table.
//
// Not thread safe: the owner's lock covers it as it covers the table.
class KeptSetUsers {
public:
    struct Stats {
        std::uint64_t passes = 0;  ///< thinning passes
        std::uint64_t scanned = 0; ///< entries the passes went over
        std::uint64_t removed = 0; ///< stale entries they removed
        std::uint64_t longest = 0; ///< longest list a pass went over
    };

    /// A set kept under (hash, check) holds `handles` (repeats, 0 and `skip` are left out:
    /// `skip` is a handle that lives as long as the owner and is never dropped).
    void Add(std::uint64_t hash, std::uint64_t check, std::span<const std::uint64_t> handles,
             std::uint64_t skip) {
        const std::uint32_t id = NewId(hash, check);
        const Entry entry{id, serials[id]};
        scratch.assign(handles.begin(), handles.end());
        std::sort(scratch.begin(), scratch.end());
        scratch.erase(std::unique(scratch.begin(), scratch.end()), scratch.end());
        for (const std::uint64_t handle : scratch) {
            if (handle == 0 || handle == skip) continue;
            auto& list = lists[handle];
            if (list.size() > 2 * kept + 4096) Thin(list);
            list.push_back(entry);
        }
    }

    /// The handle is about to be destroyed: erase(hash, check) for each kept set that holds it,
    /// once each; those sets are no longer kept, and the handle has no list any more. False
    /// when the handle had no list.
    template <class Erase>
    bool Drop(std::uint64_t handle, Erase&& erase) {
        const auto found = lists.find(handle);
        if (found == lists.end()) return false;
        for (const Entry& entry : found->second) {
            if (serials[entry.id] != entry.serial) continue;
            const Record record = records[entry.id];
            FreeId(entry.id);
            erase(record.hash, record.check);
        }
        lists.erase(found);
        return true;
    }

    void Clear() {
        lists.clear();
        records.clear();
        serials.clear();
        free_ids.clear();
        kept = 0;
    }

    /// Sets added and not dropped since the last Clear.
    std::size_t Kept() const noexcept {
        return kept;
    }

    /// Entries in a handle's list, current or stale (tests, diagnostics).
    std::size_t ListSize(std::uint64_t handle) const {
        const auto found = lists.find(handle);
        return found == lists.end() ? 0 : found->second.size();
    }

    /// The counts since the last call.
    Stats TakeStats() noexcept {
        const Stats taken = stats;
        stats = {};
        return taken;
    }

private:
    struct Entry {
        std::uint32_t id;
        std::uint32_t serial;
    };
    struct Record {
        std::uint64_t hash;
        std::uint64_t check;
    };

    std::uint32_t NewId(std::uint64_t hash, std::uint64_t check) {
        std::uint32_t id;
        if (!free_ids.empty()) {
            id = free_ids.back();
            free_ids.pop_back();
        } else {
            id = static_cast<std::uint32_t>(records.size());
            records.push_back({});
            serials.push_back(0);
        }
        records[id] = {hash, check};
        ++kept;
        return id;
    }

    void FreeId(std::uint32_t id) {
        ++serials[id]; // the id's entries in every list are stale from now on
        free_ids.push_back(id);
        --kept;
    }

    void Thin(std::vector<Entry>& list) {
        ++stats.passes;
        stats.scanned += list.size();
        stats.longest = std::max<std::uint64_t>(stats.longest, list.size());
        const std::size_t before = list.size();
        std::erase_if(list, [&](const Entry& e) { return serials[e.id] != e.serial; });
        stats.removed += before - list.size();
    }

    std::unordered_map<std::uint64_t, std::vector<Entry>> lists;
    std::vector<Record> records;       ///< by id: the kept set's key
    std::vector<std::uint32_t> serials; ///< by id: bumped when the id's set is dropped
    std::vector<std::uint32_t> free_ids;
    std::vector<std::uint64_t> scratch;
    std::size_t kept = 0;
    Stats stats;
};
} // namespace Native
