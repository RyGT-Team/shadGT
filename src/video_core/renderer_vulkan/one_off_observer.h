// Copyright 2026 RyGT team. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace Vulkan {
struct OneOffKey {
    std::uint64_t address{};
    std::uint32_t pitch{}, height{}, first_slice{}, slices{}, samples{}, format{}, number{}, tile{};
    bool depth{};
    bool operator==(const OneOffKey&) const = default;
};

// Only adds waits: a skipped output sampled before a redraw is remembered by
// exact address and format. Missing observations never relax existing guards.
class OneOffObserver {
public:
    static constexpr std::size_t Limit = 1024;
    explicit OneOffObserver(std::filesystem::path path_) : path(std::move(path_)) {
        try {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (!ec && size <= MaxFileBytes) {
                std::ifstream in(path);
                std::string line;
                while (learned.size() < Limit && std::getline(in, line)) {
                    std::istringstream row(line);
                    OneOffKey key{};
                    unsigned depth{};
                    if (row >> key.address >> key.pitch >> key.height >> key.first_slice >> key.slices >>
                            key.samples >> key.format >> key.number >> key.tile >> depth &&
                        depth <= 1 && key.address && key.pitch && key.height && key.slices && key.samples) {
                        key.depth = depth != 0;
                        if (std::ranges::find(learned, key) == learned.end()) learned.push_back(key);
                    }
                }
            }
            has_learned.store(!learned.empty(), std::memory_order_release);
            writer = std::jthread([this](std::stop_token stop) { WriteLoop(stop); });
        } catch (...) {
            // Persistence is optional. In-memory safety observations still work.
        }
    }
    ~OneOffObserver() {
        if (writer.joinable()) {
            writer.request_stop();
            cv.notify_all();
            writer.join();
        }
    }
    OneOffObserver(const OneOffObserver&) = delete;
    OneOffObserver& operator=(const OneOffObserver&) = delete;

    void Skipped(const OneOffKey& key) {
        if (!key.address) return;
        try {
            std::scoped_lock lock{mutex};
            if (std::ranges::find(watched, key) == watched.end()) {
                if (watched.size() == Limit) watched.erase(watched.begin());
                watched.push_back(key);
            }
            has_watches.store(!watched.empty(), std::memory_order_release);
        } catch (const std::bad_alloc&) {}
    }
    void Redrawn(const OneOffKey& key) {
        if (!has_watches.load(std::memory_order_acquire)) return;
        std::scoped_lock lock{mutex};
        std::erase(watched, key);
        has_watches.store(!watched.empty(), std::memory_order_release);
    }
    bool HasWatches() const { return has_watches.load(std::memory_order_acquire); }
    void Sampled(std::uint64_t address) {
        if (!has_watches.load(std::memory_order_acquire)) return;
        try {
            std::scoped_lock lock{mutex};
            for (auto it = watched.begin(); it != watched.end();) {
                if (it->address != address) { ++it; continue; }
                if (learned.size() < Limit && std::ranges::find(learned, *it) == learned.end()) {
                    learned.push_back(*it);
                    has_learned.store(true, std::memory_order_release);
                    if (pending.size() < Limit) pending.push_back(*it);
                }
                it = watched.erase(it);
            }
            has_watches.store(!watched.empty(), std::memory_order_release);
            cv.notify_one();
        } catch (const std::bad_alloc&) {}
    }
    bool MustWait(const OneOffKey& key) const {
        if (!has_learned.load(std::memory_order_acquire)) return false;
        std::scoped_lock lock{mutex};
        return std::ranges::find(learned, key) != learned.end();
    }

private:
    void WriteLoop(std::stop_token stop) noexcept {
        try {
            for (;;) {
                std::vector<OneOffKey> batch;
                {
                    std::unique_lock lock{mutex};
                    cv.wait(lock, stop, [&] { return !pending.empty(); });
                    batch.swap(pending);
                    if (batch.empty() && stop.stop_requested()) return;
                }
                std::error_code ec;
                const auto size = std::filesystem::file_size(path, ec);
                if (ec && std::filesystem::exists(path)) continue;
                std::uintmax_t used = ec ? 0 : size;
                if (used >= MaxFileBytes) continue;
                std::ofstream out(path, std::ios::app);
                for (const auto& k : batch) {
                    std::ostringstream row;
                    row << k.address << ' ' << k.pitch << ' ' << k.height << ' ' << k.first_slice << ' '
                        << k.slices << ' ' << k.samples << ' ' << k.format << ' ' << k.number << ' '
                        << k.tile << ' ' << unsigned(k.depth) << '\n';
                    const auto text = row.str();
                    if (text.size() > MaxFileBytes - used) break;
                    out << text;
                    used += text.size();
                }
            }
        } catch (...) {
            // Preserve any existing file; failed persistence does not change rendering.
        }
    }
    static constexpr std::uintmax_t MaxFileBytes = 1024 * 1024;
    std::filesystem::path path;
    mutable std::mutex mutex;
    std::condition_variable_any cv;
    std::vector<OneOffKey> watched, learned, pending;
    std::atomic<bool> has_watches{}, has_learned{};
    std::jthread writer;
};

inline std::atomic<std::shared_ptr<OneOffObserver>> g_one_off_observer;
inline std::shared_ptr<OneOffObserver> CurrentOneOffObserver() {
    return g_one_off_observer.load(std::memory_order_acquire);
}
} // namespace Vulkan
