#include <thread>
#include <gtest/gtest.h>
#include "common/native_thread_priority.h"
#include "video_core/renderer_vulkan/native_chunk_policy.h"

TEST(NativeChunkPolicy, DefaultsOverridesAndBoundedRegionSteps) {
    constexpr std::uint64_t mb = 1024 * 1024;
    EXPECT_EQ(Vulkan::NativeChunkBytes(nullptr), mb);
    EXPECT_EQ(Vulkan::NativeChunkBytes("0"), mb);
    EXPECT_EQ(Vulkan::NativeChunkBytes("-1"), mb);
    EXPECT_EQ(Vulkan::NativeChunkBytes("8junk"), mb);
    EXPECT_EQ(Vulkan::NativeChunkBytes("9999999999999999999999999"), mb);
    EXPECT_EQ(Vulkan::NativeChunkBytes("8"), 8 * mb);
    EXPECT_EQ(Vulkan::NativeRegionUploadStep(10 * mb, mb), mb);
    EXPECT_EQ(Vulkan::NativeRegionUploadStep(10 * mb, 8 * mb), 4 * mb);
    EXPECT_EQ(Vulkan::NativeRegionUploadStep(123, mb), 123u);
}

TEST(NativeThreadPriority, RequiresExactOptIn) {
    EXPECT_FALSE(Common::ExactNativePriorityOptIn(nullptr));
    EXPECT_FALSE(Common::ExactNativePriorityOptIn(""));
    EXPECT_FALSE(Common::ExactNativePriorityOptIn("0"));
    EXPECT_FALSE(Common::ExactNativePriorityOptIn("1extra"));
    EXPECT_TRUE(Common::ExactNativePriorityOptIn("1"));
}

#ifdef _WIN32
TEST(NativeThreadPriority, RestoresOriginalAndRespectsExternalChanges) {
    std::thread thread([] {
        HANDLE current = GetCurrentThread();
        const int original = GetThreadPriority(current);
        ASSERT_NE(original, THREAD_PRIORITY_ERROR_RETURN);
        ASSERT_TRUE(SetThreadPriority(current, THREAD_PRIORITY_NORMAL));
        {
            Common::NativeThreadPriority priority;
            priority.Update(true);
            EXPECT_EQ(GetThreadPriority(current), THREAD_PRIORITY_ABOVE_NORMAL);
            priority.Update(false);
            EXPECT_EQ(GetThreadPriority(current), THREAD_PRIORITY_NORMAL);
            priority.Update(true);
            ASSERT_TRUE(SetThreadPriority(current, THREAD_PRIORITY_BELOW_NORMAL));
            priority.Update(true);
            EXPECT_EQ(GetThreadPriority(current), THREAD_PRIORITY_BELOW_NORMAL);
        }
        EXPECT_EQ(GetThreadPriority(current), THREAD_PRIORITY_BELOW_NORMAL);
        SetThreadPriority(current, original);
    });
    thread.join();
}
#endif

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
