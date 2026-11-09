#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/one_off_observer.h"

TEST(OneOffObserver, AddsOnlyExactHarmfulWaitsAndPersistsThem) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("rygt-one-off-test-" + std::to_string(suffix) + ".txt");
    const Vulkan::OneOffKey a{0x1000, 64, 32, 0, 1, 1, 4, 0, 2, false};
    auto other = a; other.format = 5;
    {
        Vulkan::OneOffObserver observer{path};
        EXPECT_FALSE(observer.MustWait(a));
        observer.Skipped(a);
        observer.Sampled(0x2000);
        EXPECT_FALSE(observer.MustWait(a));
        observer.Sampled(0x1000);
        EXPECT_TRUE(observer.MustWait(a));
        EXPECT_FALSE(observer.MustWait(other));
    }
    {
        Vulkan::OneOffObserver observer{path};
        EXPECT_TRUE(observer.MustWait(a));
        EXPECT_FALSE(observer.MustWait(other));
    }
    std::filesystem::remove(path);
}

TEST(OneOffObserver, RedrawClearsOnlyTheMatchingWatch) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("rygt-one-off-test-" + std::to_string(suffix) + ".txt");
    const Vulkan::OneOffKey a{0x1000, 64, 32, 0, 1, 1, 4, 0, 2, false};
    auto b = a; b.address = 0x2000;
    {
        Vulkan::OneOffObserver observer{path};
        observer.Skipped(a);
        observer.Skipped(b);
        observer.Redrawn(a);
        observer.Sampled(a.address);
        observer.Sampled(b.address);
        EXPECT_FALSE(observer.MustWait(a));
        EXPECT_TRUE(observer.MustWait(b));
    }
    std::filesystem::remove(path);
}

TEST(OneOffObserver, PreservesMalformedFileAndBoundsWatches) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("rygt-one-off-test-" + std::to_string(suffix) + ".txt");
    { std::ofstream file(path); file << "original malformed record\n"; }
    Vulkan::OneOffKey key{1, 64, 32, 0, 1, 1, 4, 0, 2, false};
    {
        Vulkan::OneOffObserver observer{path};
        for (std::uint64_t i = 1; i <= Vulkan::OneOffObserver::Limit + 1; ++i) {
            key.address = i;
            observer.Skipped(key);
        }
        key.address = 1;
        observer.Sampled(key.address);
        EXPECT_FALSE(observer.MustWait(key)); // oldest watch was evicted
        key.address = Vulkan::OneOffObserver::Limit + 1;
        observer.Sampled(key.address);
        EXPECT_TRUE(observer.MustWait(key));
    }
    std::ifstream file(path);
    std::string first;
    std::getline(file, first);
    EXPECT_EQ(first, "original malformed record");
    file.close();
    std::filesystem::remove(path);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
