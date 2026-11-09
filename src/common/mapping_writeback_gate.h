// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace Common {
// BeginUnmap runs before the OS mapping changes. Completion holds a shared gate
// through the actual write, so BeginUnmap waits for older writers to finish.
class MappingWritebackGate {
    struct State { std::uint64_t begin, end; bool valid; };
public:
    using Token = std::shared_ptr<State>;
    // Freeze before the renderer can create readback work. A late-loaded marker
    // cannot enable protection while an old-runtime write is already in flight.
    void FreezeProtocol(bool available) {
        unsigned expected = 0;
        protocol.compare_exchange_strong(expected, available ? 2u : 1u,
                                         std::memory_order_release, std::memory_order_relaxed);
    }
    bool ProtocolAvailable() const { return protocol.load(std::memory_order_acquire) == 2; }
    Token Capture(std::uint64_t address, std::uint64_t size) {
        // Any unexpected work before initialization conservatively freezes OFF.
        if (protocol.load(std::memory_order_acquire) == 0) FreezeProtocol(false);
        if (!ProtocolAvailable()) return {};
        std::unique_lock lock{mutex};
        if (!size || address > UINT64_MAX - size) return {};
        auto token = std::make_shared<State>(State{address, address + size, true});
        const auto range = std::lower_bound(unavailable.begin(), unavailable.end(), address,
            [](const auto& item, std::uint64_t value) { return item.second <= value; });
        if (range != unavailable.end() && range->first < address + size) token->valid = false;
        if (tokens.size() >= next_cleanup) {
            std::erase_if(tokens, [](const auto& weak) { return weak.expired(); });
            next_cleanup = std::max<std::size_t>(1024, tokens.size() * 2);
        }
        tokens.push_back(token);
        return token;
    }
    void BeginUnmap(std::uint64_t address, std::uint64_t size) {
        if (!size || address > UINT64_MAX - size) return;
        std::unique_lock lock{mutex};
        // Reserve before mutating: a failed allocation must not lose an existing
        // unavailable range. Adjacent intervals are merged as well as overlaps.
        if (unavailable.size() == unavailable.capacity()) {
            const auto max = unavailable.max_size();
            const auto capacity = unavailable.capacity();
            const auto grown = capacity > max / 2 ? max : std::max<std::size_t>(8, capacity * 2);
            unavailable.reserve(std::max(unavailable.size() + 1, grown));
        }
        auto first = std::lower_bound(unavailable.begin(), unavailable.end(), address,
            [](const auto& item, std::uint64_t value) { return item.second < value; });
        auto last = first;
        auto begin = address;
        auto end = address + size;
        while (last != unavailable.end() && last->first <= end) {
            begin = std::min(begin, last->first);
            end = std::max(end, last->second);
            ++last;
        }
        if (first == last) unavailable.insert(first, {begin, end});
        else {
            *first = {begin, end};
            unavailable.erase(first + 1, last);
        }
        for (auto& weak : tokens) if (auto token = weak.lock())
            if (token->begin < address + size && address < token->end) token->valid = false;
    }
    void PublishMap(std::uint64_t address, std::uint64_t size) {
        if (!size || address > UINT64_MAX - size) return;
        std::unique_lock lock{mutex};
        const auto end = address + size;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> next;
        next.reserve(unavailable.size() + 1);
        for (const auto& range : unavailable) {
            if (range.second <= address || range.first >= end) next.push_back(range);
            else {
                if (range.first < address) next.emplace_back(range.first, address);
                if (range.second > end) next.emplace_back(end, range.second);
            }
        }
        unavailable.swap(next);
    }
    template<class Write>
    bool Complete(const Token& token, Write write) {
        if (!ProtocolAvailable()) { write(); return true; }
        std::shared_lock lock{mutex};
        if (!token || !token->valid) return false;
        write();
        return true;
    }
private:
    std::shared_mutex mutex;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> unavailable;
    std::vector<std::weak_ptr<State>> tokens;
    std::size_t next_cleanup = 1024;
    std::atomic<unsigned> protocol{}; // 0: not frozen, 1: unavailable, 2: available
};
inline MappingWritebackGate& GuestMappingWritebackGate() {
    static MappingWritebackGate gate;
    return gate;
}
inline thread_local const MappingWritebackGate::Token* current_mapping_writeback_token = nullptr;
class ScopedMappingWritebackToken {
public:
    explicit ScopedMappingWritebackToken(const MappingWritebackGate::Token& token)
        : previous{current_mapping_writeback_token} { current_mapping_writeback_token = &token; }
    ~ScopedMappingWritebackToken() { current_mapping_writeback_token = previous; }
private:
    const MappingWritebackGate::Token* previous;
};
template<class Write>
bool CompleteScopedMappingWriteback(Write write) {
    if (!current_mapping_writeback_token) { write(); return true; }
    return GuestMappingWritebackGate().Complete(*current_mapping_writeback_token, write);
}
}
