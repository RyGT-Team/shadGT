// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <type_traits>

namespace Vulkan::SnapshotLease {
using U = uint64_t;
constexpr U GuestLimit = U{1} << 40, Granule = U{1} << 16, Page = 4096, Budget = 20U * 1024 * 1024;
struct Range {
    U address{}, size{};
};
inline bool End(Range r, U& end) {
    if (!r.size || r.address >= GuestLimit || r.size > GuestLimit - r.address)
        return false;
    end = r.address + r.size;
    return true;
}
inline std::optional<Range> Pages(Range r) {
    U end;
    if (!End(r, end))
        return {};
    U begin = r.address & ~(Page - 1);
    end = (end + Page - 1) & ~(Page - 1);
    return Range{begin, end - begin};
}

struct Mapping {
    U epoch{};
    bool idle{}, capability{}, unique_backing{};
};
inline bool Good(Mapping m) {
    return m.idle && m.capability && m.unique_backing && m.epoch;
}
struct Id {
    uint32_t slot{};
    U generation{};
};
struct Token {
    Id id{};
    U list_epoch{}, address{}, size{};
    bool valid{};
};
enum class State : uint8_t { Empty, Arming, Valid, Invalid };
struct Lease {
    Range source{}, pages{};
    U map_epoch{}, generation{};
    State state{};
};
struct OwnedRanges {
    std::array<Range, 64> ranges{};
    size_t count{};
};
struct OwnedLease {
    Id id{};
    State state{};
    U map_epoch{};
};
template <class PoisonProvider>
class StateTable {
    PoisonProvider& poison;
    std::mutex mutex;
    std::array<Lease, 64> leases{};
    U owned{}, next_generation{1};
    bool Same(Id id, const Lease& l) const {
        return id.generation && l.generation == id.generation;
    }

public:
    explicit StateTable(PoisonProvider& p) : poison(p) {}
    std::optional<Id> Find(Range r) {
        std::lock_guard lock(mutex);
        for (uint32_t i = 0; i < leases.size(); ++i) {
            const auto& l = leases[i];
            if (l.state == State::Valid && l.source.address == r.address && l.source.size == r.size)
                return Id{i, l.generation};
        }
        return {};
    }
    std::optional<OwnedLease> FindOwned(Range r) {
        std::lock_guard lock(mutex);
        for (uint32_t i = 0; i < leases.size(); ++i) {
            const auto& l = leases[i];
            if (l.state != State::Empty && l.source.address == r.address && l.source.size == r.size)
                return OwnedLease{Id{i, l.generation}, l.state, l.map_epoch};
        }
        return {};
    }
    OwnedRanges AllOwnedRanges() {
        std::lock_guard lock(mutex);
        OwnedRanges result;
        for (const auto& l : leases)
            if (l.state != State::Empty)
                result.ranges[result.count++] = l.pages;
        return result;
    }
    // Reclaim metadata only. The caller must serialize this with watch-owner
    // reconciliation and retire the returned ranges through the normal path.
    // poison=false requires caller-owned reconciliation and a protocol that
    // advances the mapping epoch before any page is opened for writing.
    OwnedRanges ReleaseUnsafe(bool poison_on_release = true) {
        std::lock_guard lock(mutex);
        OwnedRanges result;
        for (auto& lease : leases) {
            if (lease.state == State::Empty || poison.Safe(lease.pages))
                continue;
            if (poison_on_release)
                poison.MarkUnsafe(lease.pages);
            result.ranges[result.count++] = lease.pages;
            owned -= lease.pages.size;
            lease.state = State::Empty;
        }
        return result;
    }
    bool Invalidate(Id id) {
        std::lock_guard lock(mutex);
        if (id.slot >= leases.size() || !Same(id, leases[id.slot]) ||
            leases[id.slot].state == State::Empty)
            return false;
        leases[id.slot].state = State::Invalid;
        return true;
    }
    std::optional<Id> Begin(Range r, Mapping m) {
        auto pages = Pages(r);
        if (!pages || !Good(m) || !poison.Safe(*pages))
            return {};
        std::lock_guard lock(mutex);
        if (!poison.Safe(*pages) || pages->size > Budget - owned || next_generation == 0)
            return {};
        for (uint32_t i = 0; i < leases.size(); ++i)
            if (leases[i].state == State::Empty) {
                auto g = next_generation++;
                leases[i] = {r, *pages, m.epoch, g, State::Arming};
                owned += pages->size;
                return Id{i, g};
            }
        return {};
    }
    // Both mapping receipts surround protection and exact snapshot capture; the
    // caller supplies true only after successful OS protection and byte proof.
    bool Commit(Id id, Mapping before, Mapping after, bool protected_ok, bool bytes_proven) {
        std::lock_guard lock(mutex);
        if (id.slot >= leases.size())
            return false;
        auto& l = leases[id.slot];
        if (!Same(id, l) || l.state != State::Arming)
            return false;
        if (!protected_ok || !bytes_proven || !Good(before) || !Good(after) ||
            before.epoch != l.map_epoch || after.epoch != l.map_epoch || !poison.Safe(l.pages)) {
            l.state = State::Invalid;
            return false;
        }
        l.state = State::Valid;
        return true;
    }
    // Revalidate the same still-owned watch after an unrelated mapping epoch
    // change. A fresh exact capture and protection receipt are mandatory.
    // A failed attempt invalidates the old lease but never opens the watch.
    std::optional<Id> Renew(Id id, Mapping before, Mapping after, bool protected_ok,
                            bool bytes_proven) {
        std::lock_guard lock(mutex);
        if (id.slot >= leases.size())
            return {};
        auto& l = leases[id.slot];
        if (!Same(id, l) || (l.state != State::Valid && l.state != State::Invalid))
            return {};
        if (!protected_ok || !bytes_proven || !Good(before) || !Good(after) ||
            before.epoch != after.epoch || !poison.Safe(l.pages) || !next_generation) {
            l.state = State::Invalid;
            return {};
        }
        l.map_epoch = after.epoch;
        l.generation = next_generation++;
        l.state = State::Valid;
        return Id{id.slot, l.generation};
    }
    // Default poisoning invalidates overlapping leases. With poison=false the
    // caller must retain other owners and advance the epoch before opening pages.
    std::optional<Range> Release(Id id, bool poison_on_release = true) {
        std::lock_guard lock(mutex);
        if (id.slot >= leases.size())
            return {};
        auto& l = leases[id.slot];
        if (!Same(id, l) || l.state == State::Empty)
            return {};
        auto p = l.pages;
        if (poison_on_release)
            poison.MarkUnsafe(p);
        owned -= p.size;
        l.state = State::Empty;
        return p;
    }
    Token Bind(Id id, U list_epoch, Range r, Mapping before, Mapping after, bool stack_allowed,
               bool bytes_proven) {
        std::lock_guard lock(mutex);
        if (id.slot >= leases.size() || !list_epoch)
            return {};
        auto& l = leases[id.slot];
        if (!Same(id, l) || l.state != State::Valid || l.source.address != r.address ||
            l.source.size != r.size || !Good(before) || !Good(after) ||
            before.epoch != l.map_epoch || after.epoch != l.map_epoch || !stack_allowed ||
            !bytes_proven || !poison.Safe(l.pages))
            return {};
        return {id, list_epoch, r.address, r.size, true};
    }
    bool Reuse(Token t, U list_epoch, Range r, Mapping m, bool stack_allowed) {
        std::lock_guard lock(mutex);
        if (!t.valid || !list_epoch || t.list_epoch != list_epoch || t.address != r.address ||
            t.size != r.size || t.id.slot >= leases.size())
            return false;
        auto& l = leases[t.id.slot];
        return Same(t.id, l) && l.state == State::Valid && l.source.address == r.address &&
               l.source.size == r.size && Good(m) && m.epoch == l.map_epoch && stack_allowed &&
               poison.Safe(l.pages);
    }
    // Runtime mapping receipts must bracket the first poison/lease check. A
    // final poison check catches writes concurrent with the second receipt.
    // This depends on a monotonic epoch and an active-writer protocol supplied
    // by the runtime; this component cannot prove those external guarantees.
    template <class ReadMapping>
    bool ReuseChecked(Token t, U list_epoch, Range r, bool stack_allowed,
                      ReadMapping&& read_mapping) {
        const Mapping before = read_mapping();
        if (!Reuse(t, list_epoch, r, before, stack_allowed))
            return false;
        const Mapping after = read_mapping();
        return Good(before) && Good(after) && before.epoch == after.epoch &&
               Reuse(t, list_epoch, r, after, stack_allowed);
    }
};
class ListTokens {
    std::array<Token, 8> tokens{};
    U epoch{1};

public:
    U Epoch() const {
        return epoch;
    }
    void Reset() {
        tokens = {};
        if (epoch)
            ++epoch;
    }
    void Set(unsigned slot, Token t) {
        if (slot < tokens.size())
            tokens[slot] = t;
    }
    Token Get(unsigned slot) const {
        return slot < tokens.size() ? tokens[slot] : Token{};
    }
    void Forget(unsigned slot) {
        if (slot < tokens.size())
            tokens[slot] = {};
    }
};
} // namespace Vulkan::SnapshotLease
