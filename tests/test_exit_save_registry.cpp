// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <chrono>
#include <semaphore>
#include <thread>
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/exit_save_registry.h"

TEST(ExitSaveRegistry, UnregisterWaitsUntilSaveReleasesOwner) {
    using namespace std::chrono_literals;
    struct Owner { int value=42; } owner;
    Vulkan::ExitSaveRegistry<Owner> registry;
    ASSERT_TRUE(registry.Register(&owner));
    std::binary_semaphore entered{0}, release{0}, removed{0};
    std::jthread save([&] {
        EXPECT_TRUE(registry.Invoke([&](Owner& target) {
            EXPECT_EQ(target.value,42); entered.release(); release.acquire();
            EXPECT_EQ(target.value,42);
        }));
    });
    entered.acquire();
    std::jthread destroy([&] { registry.Unregister(&owner); removed.release(); });
    EXPECT_FALSE(removed.try_acquire_for(100ms));
    release.release();
    EXPECT_TRUE(removed.try_acquire_for(2s));
    EXPECT_FALSE(registry.Invoke([](Owner&) {}));
}

TEST(ExitSaveRegistry, ExceptionReleasesGuardAndDuplicateDoesNotReplaceOwner) {
    struct Owner {} first, second;
    Vulkan::ExitSaveRegistry<Owner> registry;
    ASSERT_TRUE(registry.Register(&first));
    EXPECT_FALSE(registry.Register(&second));
    EXPECT_THROW(registry.Invoke([](Owner&) { throw 7; }), int);
    registry.Unregister(&second);
    EXPECT_TRUE(registry.Invoke([&](Owner& owner) { EXPECT_EQ(&owner,&first); }));
    registry.Unregister(&first);
    EXPECT_FALSE(registry.Invoke([](Owner&) {}));
}
