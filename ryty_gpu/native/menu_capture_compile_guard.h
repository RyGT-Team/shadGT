#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>

namespace Native {

// A capture remains active until its producer has ended and its one save succeeded.
// Fixed storage avoids allocation failure in hooks; ambiguity permanently fails closed.
class MenuCaptureCompileGuard {
public:
    using Token = std::uint64_t;

    void ConfigureReady(bool ready) {
        std::scoped_lock lock{mutex_};
        if (configured_ || !ready) {
            Fail();
            configured_ = true;
            return;
        }
        configured_ = true;
        ready_.store(true, std::memory_order_release);
    }

    bool Begin(const void* controller) {
        std::scoped_lock lock{mutex_};
        if (!configured_ || failed_.load(std::memory_order_relaxed) || !ready_.load(std::memory_order_relaxed) || !controller ||
            FindController(controller) || next_generation_ == (std::numeric_limits<Token>::max)()) {
            Fail();
            return false;
        }
        for (auto& record : records_) {
            if (!record.controller) {
                record = Record{.controller = controller, .generation = ++next_generation_};
                active_.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
        Fail();
        return false;
    }

    bool Attach(const void* controller, const void* object, const void* owner = nullptr) {
        std::scoped_lock lock{mutex_};
        auto* record = FindController(controller);
        if (!record || !object || record->object || FindObject(object)) {
            Fail();
            return false;
        }
        record->object = object;
        record->owner = owner;
        return true;
    }

    Token StartSave(const void* object) {
        std::scoped_lock lock{mutex_};
        auto* record = FindObject(object);
        if (!record || record->save_started) {
            Fail();
            return 0;
        }
        record->save_started = true;
        return record->generation;
    }

    void CompleteSave(const void* object, Token token, bool success) {
        std::scoped_lock lock{mutex_};
        auto* record = FindObject(object);
        if (!record || !token || record->generation != token || !record->save_started ||
            record->save_completed || !success) {
            Fail();
            return;
        }
        record->save_completed = true;
        save_cv_.notify_all();
    }

    bool WaitForSave(const void* controller, std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        auto* record = FindController(controller);
        if (!record || !record->object || !record->save_started) {
            Fail();
            return false;
        }
        const Token generation = record->generation;
        const bool completed = save_cv_.wait_for(lock, timeout, [&] {
            auto* current = FindController(controller);
            return failed_.load(std::memory_order_acquire) || !current ||
                   current->generation != generation || current->save_completed;
        });
        record = FindController(controller);
        if (!completed || failed_.load(std::memory_order_acquire) || !record ||
            record->generation != generation || !record->save_completed) {
            Fail();
            return false;
        }
        return true;
    }

    bool End(const void* controller, const void* expected_object = nullptr,
             const void* expected_owner = nullptr) {
        std::scoped_lock lock{mutex_};
        auto* record = FindController(controller);
        if (failed_.load(std::memory_order_relaxed) || !record || !record->object ||
            (expected_object && record->object != expected_object) ||
            (record->owner != expected_owner) ||
            !record->save_started || !record->save_completed) {
            Fail();
            return false;
        }
        *record = {};
        active_.fetch_sub(1, std::memory_order_release);
        return true;
    }

    bool CanContinue() const noexcept {
        return ready_.load(std::memory_order_acquire) &&
               !failed_.load(std::memory_order_acquire) &&
               active_.load(std::memory_order_acquire) == 0;
    }

private:
    struct Record {
        const void* controller{};
        const void* object{};
        const void* owner{};
        Token generation{};
        bool save_started{};
        bool save_completed{};
    };
    Record* FindController(const void* controller) {
        if (!controller) return nullptr;
        for (auto& record : records_) if (record.controller == controller) return &record;
        return nullptr;
    }
    Record* FindObject(const void* object) {
        if (!object) return nullptr;
        for (auto& record : records_) if (record.controller && record.object == object) return &record;
        return nullptr;
    }
    void Fail() noexcept {
        failed_.store(true, std::memory_order_release);
        save_cv_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable save_cv_;
    std::array<Record, 64> records_{};
    Token next_generation_{};
    bool configured_{};
    std::atomic<bool> ready_{};
    std::atomic<bool> failed_{};
    std::atomic<unsigned> active_{};
};

} // namespace Native
