#include <array>
#include <cstdlib>
#include <span>
#include <vector>
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/vk_command_stream.h"
#include "video_core/renderer_vulkan/native_ring_policy.h"

// This isolated test replays Slot commands without constructing a GPU scheduler.
namespace Vulkan {
class Scheduler {};
void CommandRing::WaitForSpace(size_t) { std::abort(); }
vk::CommandBuffer ReplayCommandBuffer(Scheduler&, CmdTarget) { std::abort(); }
}
void assert_fail_impl() { std::abort(); }
namespace Common::Log {
void VLog(Class, Level, const char*, int, const char*, fmt::string_view, fmt::format_args) {}
}

TEST(NativeCommandRing, ReplaysInOrderWithCopiedPayloadAndLateSlot) {
    Vulkan::CommandRing ring;
    Vulkan::Scheduler scheduler;
    vk::CommandBuffer slot{};
    Vulkan::CmdBuf cmd{&ring, &slot};
    std::vector<uintptr_t> observed;
    cmd.Invoke([&](vk::CommandBuffer c) { observed.push_back(reinterpret_cast<uintptr_t>(static_cast<VkCommandBuffer>(c))); });
    std::array<int, 3> input{3, 5, 8};
    ring.Record<Vulkan::CmdTarget::Slot>(Vulkan::CommandRing::Bytes<int>(input.size()),
        [&](Vulkan::CommandRing::Extra& extra) {
            const auto* copy = extra.Copy(input.data(), input.size());
            return [&, copy](vk::CommandBuffer) { for (int n : std::span(copy, input.size())) observed.push_back(n); };
        });
    input.fill(0);
    slot = vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(uintptr_t{0x1234})};
    u64 cursor = 0;
    while (cursor < ring.Published()) {
        auto* header = ring.At(cursor);
        ASSERT_NE(header->exec, nullptr);
        header->exec(scheduler, reinterpret_cast<u8*>(header) + header->fn_offset);
        cursor += header->size;
        ring.SetReplayed(cursor);
    }
    EXPECT_EQ(observed, (std::vector<uintptr_t>{0x1234, 3, 5, 8}));
}

TEST(NativeCommandRing, ComparisonAndIdleRespectExplicitOffAndExclusions) {
    using Vulkan::NativeRingWindow;
    EXPECT_FALSE(NativeRingWindow(false, true, true, 1));
    EXPECT_TRUE(NativeRingWindow(true, false, true, 0));
    EXPECT_FALSE(NativeRingWindow(true, true, true, 0));
    EXPECT_TRUE(NativeRingWindow(true, true, true, 1));
    EXPECT_TRUE(Vulkan::NativeBeginAheadAllowed(true, false, false, false, false, false));
    EXPECT_FALSE(Vulkan::NativeBeginAheadAllowed(true, false, false, false, false, true));
    EXPECT_FALSE(Vulkan::NativeBeginAheadAllowed(true, true, false, false, false, false));
    EXPECT_FALSE(Vulkan::NativeBeginAheadAllowed(true, false, true, false, false, false));
    EXPECT_FALSE(Vulkan::NativeBeginAheadAllowed(true, false, false, true, false, false));
    EXPECT_FALSE(Vulkan::NativeBeginAheadAllowed(true, false, false, false, true, false));
}
