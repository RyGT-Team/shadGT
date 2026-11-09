#include <array>
#include <bit>
#include <cstring>
#include <atomic>
#include <future>
#include <mutex>
#include <thread>
#include <gtest/gtest.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "shader_recompiler/params.h"
#include "video_core/amdgpu/regs_shader.h"
#include "video_core/amdgpu/regs_vertex.h"
#include "video_core/renderer_vulkan/guarded_shader_snapshot.h"
#include "video_core/renderer_vulkan/retry_input_policy.h"
#include "video_core/renderer_vulkan/read_ahead_retry_queue.h"
#include "video_core/renderer_vulkan/read_ahead_retry_inputs.h"
#include "video_core/renderer_vulkan/required_pipeline_wait.h"

namespace {
struct TestInfo {
    std::uint32_t magic{};
    std::uint32_t length{};
    std::uint64_t shader_hash{};
    bool Valid() const { return magic == 0x12345678; }
};
}

TEST(GuardedShaderSnapshot, PreservesBaseAndCopiesCode) {
    std::array<std::uint32_t, 16> guest{};
    const TestInfo info{0x12345678, sizeof(guest), 0x4567};
    std::memcpy(guest.data() + 4, &info, sizeof(info));
    const auto base = reinterpret_cast<std::uintptr_t>(guest.data());
    const auto read = [&](void* out, std::uintptr_t addr, std::size_t size) {
        if (addr < base || addr - base > sizeof(guest) || size > sizeof(guest) - (addr - base))
            return false;
        std::memcpy(out, reinterpret_cast<const void*>(addr), size);
        return true;
    };
    const auto captured = Vulkan::CaptureShaderSnapshot<TestInfo>(base, read);
    ASSERT_TRUE(captured);
    EXPECT_EQ(captured->guest_base, base);
    EXPECT_EQ(captured->hash, info.shader_hash);
    EXPECT_EQ(captured->code.size(), guest.size());
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user{};
    const auto params = Vulkan::MakeOwnedShaderParams(user, *captured);
    EXPECT_EQ(params.Base(), base);
    guest[0] = 99;
    EXPECT_NE(captured->code[0], guest[0]);
}

TEST(GuardedShaderSnapshot, RejectsUnreadableAndChangingMetadata) {
    std::array<std::uint32_t, 16> guest{};
    TestInfo info{0x12345678, sizeof(guest), 1};
    std::memcpy(guest.data() + 4, &info, sizeof(info));
    const auto base = reinterpret_cast<std::uintptr_t>(guest.data());
    int full_reads = 0;
    const auto read = [&](void* out, std::uintptr_t addr, std::size_t size) {
        if (addr < base || addr - base > sizeof(guest) || size > sizeof(guest) - (addr - base))
            return false;
        if (size == sizeof(guest) && ++full_reads == 1) {
            info.shader_hash = 2;
            std::memcpy(guest.data() + 4, &info, sizeof(info));
        }
        std::memcpy(out, reinterpret_cast<const void*>(addr), size);
        return true;
    };
    EXPECT_FALSE(Vulkan::CaptureShaderSnapshot<TestInfo>(base, read));
    EXPECT_FALSE(Vulkan::CaptureShaderSnapshot<TestInfo>(base, [](void*, std::uintptr_t, std::size_t) {
        return false;
    }));
}

TEST(GuardedShaderSnapshot, ReadsRealBinaryInfoWithMetadataPastCode) {
    std::array<std::uint32_t, 64> guest{};
    AmdGpu::BinaryInfo info{};
    info.signature = AmdGpu::BinaryInfo::signature_ref;
    info.length = 32;
    info.shader_hash = 0x12345678;
    std::memcpy(guest.data() + 16, &info, sizeof(info));
    const auto base = reinterpret_cast<std::uintptr_t>(guest.data());
    auto read = [&](void* out, std::uintptr_t address, std::size_t size) {
        if (address < base || address - base > sizeof(guest) ||
            size > sizeof(guest) - (address - base)) return false;
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    };
    const auto snapshot = Vulkan::CaptureShaderSnapshot<AmdGpu::BinaryInfo>(base, read);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->guest_base, base);
    EXPECT_EQ(snapshot->hash, info.shader_hash);
    EXPECT_EQ(snapshot->code.size() * sizeof(u32), static_cast<u32>(info.length));
}

TEST(GuardedShaderSnapshot, RejectsWalkersAndFetchInputs) {
    EXPECT_TRUE(Vulkan::DirectOnlyRetryInputs(false, false, false));
    for (unsigned mask = 1; mask < 8; ++mask) {
        EXPECT_FALSE(Vulkan::DirectOnlyRetryInputs(mask & 1, mask & 2, mask & 4));
    }
    using Stages = AmdGpu::ShaderStageEnable::VgtStages;
    EXPECT_TRUE(Vulkan::DirectOnlyRetryLayout(Stages::Vs == Stages::Vs));
    EXPECT_FALSE(Vulkan::DirectOnlyRetryLayout(Stages::EsGs == Stages::Vs));
    EXPECT_FALSE(Vulkan::DirectOnlyRetryLayout(Stages::LsHs == Stages::Vs));
    EXPECT_FALSE(Vulkan::DirectOnlyRetryLayout(Stages::LsHsEsGs == Stages::Vs));
}

TEST(GuardedShaderSnapshot, RequiresExactOwnedJobCodeAndUserData) {
    std::array<u32, 2> ud{1, 2}, code{3, 4};
    auto other = code;
    EXPECT_TRUE(Vulkan::OwnedRetryJobMatches(1, 0x1000, ud, code, 1, 0x1000, ud, other, true));
    other[1] ^= 1;
    EXPECT_FALSE(Vulkan::OwnedRetryJobMatches(1, 0x1000, ud, code, 1, 0x1000, ud, other, true));
    EXPECT_FALSE(Vulkan::OwnedRetryJobMatches(1, 0x1000, ud, code, 2, 0x1000, ud, code, true));
    EXPECT_FALSE(Vulkan::OwnedRetryJobMatches(1, 0x1000, ud, code, 1, 0x2000, ud, code, true));
    EXPECT_FALSE(Vulkan::OwnedRetryJobMatches(1, 0x1000, ud, code, 1, 0x1000, ud, code, false));
}

TEST(ReadAheadRetryQueue, BoundsExpiryAndOwnsCopiedState) {
    using Clock = std::chrono::steady_clock;
    struct State {
        std::vector<u32> words;
        std::size_t bytes{};
        Clock::time_point first;
        Clock::time_point next;
    };
    Vulkan::ReadAheadRetryQueue<State> queue;
    const auto now = Clock::time_point{};
    std::vector<u32> original{1, 2, 3};
    for (u32 i = 0; i < 513; ++i) {
        State state{original, original.size() * sizeof(u32), now,
                    now + std::chrono::milliseconds{5}};
        state.words[0] = i;
        ASSERT_TRUE(queue.Keep(std::move(state)));
    }
    original[1] = 99;
    EXPECT_EQ(queue.size(), 512u);
    EXPECT_FALSE(queue.TakeReady(now));
    auto first = queue.TakeReady(now + std::chrono::milliseconds{5});
    ASSERT_TRUE(first);
    EXPECT_EQ(first->words[0], 1u);
    EXPECT_EQ(first->words[1], 2u);
    ASSERT_TRUE(queue.Retry(std::move(*first), now + std::chrono::milliseconds{5}));
    EXPECT_FALSE(queue.TakeReady(now + std::chrono::seconds{3}));
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.retained_bytes(), 0u);
    State oversized{{}, Vulkan::ReadAheadRetryQueue<State>::MaxBytes + 1, now, now};
    EXPECT_FALSE(queue.Keep(std::move(oversized)));
    const auto half = Vulkan::ReadAheadRetryQueue<State>::MaxBytes / 2 + 1;
    ASSERT_TRUE(queue.Keep(State{{10}, half, now, now}));
    ASSERT_TRUE(queue.Keep(State{{20}, half, now, now}));
    EXPECT_EQ(queue.size(), 1u);
    EXPECT_EQ(queue.retained_bytes(), half);
    auto kept = queue.TakeReady(now);
    ASSERT_TRUE(kept);
    EXPECT_EQ(kept->words[0], 20u);
}

TEST(ReadAheadRetryQueue, CopiesActualRegisterRangesBeforeSourceChanges) {
    using Ranges = AmdGpu::ReadAheadStates;
    std::vector<u32> regs(Ranges::RegisterFileDwords);
    for (size_t i = 0; i < regs.size(); ++i) regs[i] = static_cast<u32>(i + 1);
    const auto saved = Vulkan::CaptureRetryRegisters(regs);
    ASSERT_EQ(saved.size(), Ranges::StateDwords);
    std::fill(regs.begin(), regs.end(), 0);
    ASSERT_TRUE(Vulkan::RestoreRetryRegisters(regs, saved));
    for (const auto& range : Ranges::Ranges) {
        for (u32 i = 0; i < range.count; ++i) {
            EXPECT_EQ(regs[range.first + i], range.first + i + 1);
        }
    }
    EXPECT_EQ(regs[0], 0u);
    EXPECT_FALSE(Vulkan::RestoreRetryRegisters(regs, std::span<const u32>{saved}.first(1)));
}

TEST(ReadAheadRetryQueue, RequiredWaitAllowsRetryWithoutLosingCompletion) {
    std::mutex lookup;
    unsigned waiters = 0;
    std::promise<void> completion;
    auto ready = completion.get_future().share();
    std::promise<void> first_retry;
    auto observed = first_retry.get_future();
    std::atomic<unsigned> retries{};
    std::thread recorder([&] {
        std::unique_lock lock{lookup};
        Vulkan::WaitOutsideLookup(lock, waiters, [&] {
            while (ready.wait_for(std::chrono::milliseconds{1}) != std::future_status::ready) {
                if (lock.try_lock()) {
                    if (retries.fetch_add(1) == 0) first_retry.set_value();
                    lock.unlock();
                }
            }
        });
        EXPECT_TRUE(lock.owns_lock());
        EXPECT_EQ(waiters, 0u);
    });
    const auto status = observed.wait_for(std::chrono::seconds{2});
    completion.set_value(); // Always release the worker even when the check fails.
    recorder.join();
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_GT(retries.load(), 0u);
}

#ifdef _WIN32
TEST(GuardedShaderSnapshot, CodeSurvivesGuestUnmapping) {
    auto* guest = static_cast<u32*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    ASSERT_NE(guest, nullptr);
    AmdGpu::BinaryInfo info{};
    info.signature = AmdGpu::BinaryInfo::signature_ref;
    info.length = 32;
    info.shader_hash = 123;
    std::memcpy(guest + 16, &info, sizeof(info));
    guest[0] = 0x77777777;
    const auto base = reinterpret_cast<std::uintptr_t>(guest);
    auto read = [&](void* out, std::uintptr_t address, std::size_t size) {
        if (address < base || address - base > 4096 || size > 4096 - (address - base)) return false;
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    };
    const auto snapshot = Vulkan::CaptureShaderSnapshot<AmdGpu::BinaryInfo>(base, read);
    ASSERT_TRUE(snapshot);
    ASSERT_TRUE(VirtualFree(guest, 0, MEM_RELEASE));
    EXPECT_EQ(snapshot->code[0], 0x77777777u);
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user{};
    const auto params = Vulkan::MakeOwnedShaderParams(user, *snapshot);
    EXPECT_EQ(params.Base(), base);
}
#endif

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
