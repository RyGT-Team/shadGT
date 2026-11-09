#include "common/allocation_pressure_retry.h"

#include <iostream>
#include <stdexcept>
#include <vector>

enum class Status { Success, DeviceMemory, HostMemory, DeviceLost, BudgetWarning };
struct Result {
    Status result;
    unsigned handle;
};

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int main() {
    unsigned attempts = 0, reclaims = 0, live_bindings = 7;
    std::vector<Result> responses;
    auto allocate = [&] {
        Check(live_bindings == 7, "allocation recovery must not discard live bindings");
        return responses.at(attempts++);
    };
    auto reclaim = [&] {
        ++reclaims;
        Check(attempts == 1, "recovery must follow exactly one failed attempt");
    };
    responses = {{Status::Success, 42}};
    auto result = Common::AllocateWithPressureRetry(allocate, reclaim, Status::DeviceMemory);
    Check(result.handle == 42 && attempts == 1 && reclaims == 0,
          "successful allocations must not trigger cache destruction");
    attempts = reclaims = 0;
    responses = {{Status::DeviceMemory, 0}, {Status::Success, 43}};
    result = Common::AllocateWithPressureRetry(allocate, reclaim, Status::DeviceMemory);
    Check(result.handle == 43 && attempts == 2 && reclaims == 1,
          "recoverable pressure must reclaim once and preserve the successful handle");
    attempts = reclaims = 0;
    responses = {{Status::DeviceMemory, 0}, {Status::DeviceMemory, 0}};
    result = Common::AllocateWithPressureRetry(allocate, reclaim, Status::DeviceMemory);
    Check(result.result == Status::DeviceMemory && attempts == 2 && reclaims == 1,
          "persistent exhaustion must not create an unbounded retry loop");
    for (auto failure : {Status::DeviceLost, Status::HostMemory}) {
        attempts = reclaims = 0;
        responses = {{failure, 0}};
        result = Common::AllocateWithPressureRetry(allocate, reclaim, Status::DeviceMemory);
        Check(result.result == failure && attempts == 1 && reclaims == 0,
              "unrelated failures must not be hidden by GPU-cache recovery");
    }
    attempts = reclaims = 0;
    responses = {{Status::BudgetWarning, 0}, {Status::Success, 44}};
    result = Common::AllocateWithPressureRetry(allocate, reclaim, Status::BudgetWarning);
    Check(result.handle == 44 && attempts == 2 && reclaims == 1,
          "Streamline budget warnings must use bounded recovery too");
    attempts = reclaims = 0;
    responses = {{Status::DeviceMemory, 0}};
    bool propagated = false;
    try {
        Common::AllocateWithPressureRetry(
            allocate, [] { throw std::runtime_error("cleanup"); }, Status::DeviceMemory);
    } catch (const std::runtime_error&) {
        propagated = true;
    }
    Check(propagated && attempts == 1, "failed cleanup must not retry against unsafe state");
    std::cout << "PASS: allocation success, recovery, persistent OOM, unrelated errors and "
                 "cleanup failure; live bindings preserved\n";
}
