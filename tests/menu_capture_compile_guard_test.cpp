#include "ryty_gpu/native/menu_capture_compile_guard.h"
#include "ryty_gpu/native/menu_compile_policy.h"

#include <array>
#include <thread>
#include <gtest/gtest.h>

using Native::MenuCaptureCompileGuard;

TEST(MenuCompilePolicy, KeepGoingMenusRequireVerifiedIdleCaptureHooks) {
    for (unsigned mode = 0; mode != 3; ++mode) {
        EXPECT_TRUE(Native::MenuCompileMustWait(mode, false, false, false));
        EXPECT_EQ(Native::MenuCompileMustWait(mode, false, true, true), mode != 2);
        EXPECT_FALSE(Native::MenuCompileMustWait(mode, true, false, false));
        EXPECT_FALSE(Native::MenuCompileMustWait(mode, true, true, true));
        EXPECT_TRUE(Native::MenuCompileMustWait(mode, false, true, false));
        EXPECT_TRUE(Native::MenuCompileMustWait(mode, true, true, false));
    }
}

TEST(MenuCaptureCompileGuard, OwnerIdentityAndFailedStatePreventPinRelease) {
    int controller, object, owner, foreign_owner;
    MenuCaptureCompileGuard guard;
    guard.ConfigureReady(true);
    ASSERT_TRUE(guard.Begin(&controller));
    ASSERT_TRUE(guard.Attach(&controller, &object, &owner));
    auto token = guard.StartSave(&object);
    guard.CompleteSave(&object, token, true);
    EXPECT_FALSE(guard.End(&controller, &object, &foreign_owner));
    EXPECT_FALSE(guard.Begin(&foreign_owner));
    EXPECT_FALSE(guard.End(&controller, &object, &owner));
    EXPECT_FALSE(guard.CanContinue());
}

TEST(MenuCaptureCompileGuard, CompletionDoesNotReleaseBeforeEnd) {
    MenuCaptureCompileGuard guard;
    int controller, object;
    EXPECT_FALSE(guard.CanContinue());
    guard.ConfigureReady(true);
    ASSERT_TRUE(guard.CanContinue());
    guard.Begin(&controller);
    guard.Attach(&controller, &object);
    auto token = guard.StartSave(&object);
    ASSERT_NE(token, 0);
    guard.CompleteSave(&object, token, true);
    EXPECT_FALSE(guard.CanContinue());
    guard.End(&controller);
    EXPECT_TRUE(guard.CanContinue());
}

TEST(MenuCaptureCompileGuard, ConcurrentDifferentControllersRemainActiveUntilBothEnd) {
    MenuCaptureCompileGuard guard;
    guard.ConfigureReady(true);
    std::array<int, 2> controllers{}, objects{};
    std::array<std::thread, 2> threads;
    for (int i = 0; i < 2; ++i) threads[i] = std::thread([&, i] {
        guard.Begin(&controllers[i]);
        guard.Attach(&controllers[i], &objects[i]);
    });
    for (auto& thread : threads) thread.join();
    EXPECT_FALSE(guard.CanContinue());
    for (int i = 0; i < 2; ++i) {
        auto token = guard.StartSave(&objects[i]);
        ASSERT_NE(token, 0);
        guard.CompleteSave(&objects[i], token, true);
        guard.End(&controllers[i]);
        EXPECT_EQ(guard.CanContinue(), i == 1);
    }
}

TEST(MenuCaptureCompileGuard, EarlyEndFailsPermanently) {
    MenuCaptureCompileGuard guard;
    int controller, object;
    guard.ConfigureReady(true);
    guard.Begin(&controller);
    guard.Attach(&controller, &object);
    auto token = guard.StartSave(&object);
    guard.End(&controller);
    guard.CompleteSave(&object, token, true);
    guard.End(&controller);
    EXPECT_FALSE(guard.CanContinue());
}

TEST(MenuCaptureCompileGuard, NoSaveAndDuplicateSaveFailClosed) {
    int controller, object;
    MenuCaptureCompileGuard no_save;
    no_save.ConfigureReady(true);
    no_save.Begin(&controller);
    no_save.Attach(&controller, &object);
    no_save.End(&controller);
    EXPECT_FALSE(no_save.CanContinue());
    MenuCaptureCompileGuard duplicate;
    duplicate.ConfigureReady(true);
    duplicate.Begin(&controller);
    duplicate.Attach(&controller, &object);
    auto token = duplicate.StartSave(&object);
    EXPECT_EQ(duplicate.StartSave(&object), 0);
    duplicate.CompleteSave(&object, token, true);
    duplicate.End(&controller);
    EXPECT_FALSE(duplicate.CanContinue());
}

TEST(MenuCaptureCompileGuard, ReusedObjectRejectsOldGeneration) {
    MenuCaptureCompileGuard guard;
    int first, second, object;
    guard.ConfigureReady(true);
    guard.Begin(&first);
    guard.Attach(&first, &object);
    auto old = guard.StartSave(&object);
    guard.CompleteSave(&object, old, true);
    guard.End(&first);
    guard.Begin(&second);
    guard.Attach(&second, &object);
    auto current = guard.StartSave(&object);
    ASSERT_NE(old, current);
    guard.CompleteSave(&object, old, true);
    guard.CompleteSave(&object, current, true);
    guard.End(&second);
    EXPECT_FALSE(guard.CanContinue());
}

TEST(MenuCaptureCompileGuard, ReusedActiveObjectAndInstallationFailureFailClosed) {
    int first, second, object;
    MenuCaptureCompileGuard guard;
    guard.ConfigureReady(true);
    guard.Begin(&first);
    guard.Attach(&first, &object);
    guard.Begin(&second);
    guard.Attach(&second, &object);
    EXPECT_FALSE(guard.CanContinue());
    MenuCaptureCompileGuard failed;
    failed.ConfigureReady(false);
    failed.ConfigureReady(true);
    EXPECT_FALSE(failed.CanContinue());
}

TEST(MenuCaptureCompileGuard, CapacityAndFailedSaveFailClosed) {
    MenuCaptureCompileGuard guard;
    guard.ConfigureReady(true);
    std::array<int, 65> controllers{};
    for (auto& controller : controllers) guard.Begin(&controller);
    EXPECT_FALSE(guard.CanContinue());
    MenuCaptureCompileGuard failed;
    int controller, object;
    failed.ConfigureReady(true);
    failed.Begin(&controller);
    failed.Attach(&controller, &object);
    auto token = failed.StartSave(&object);
    failed.CompleteSave(&object, token, false);
    failed.End(&controller);
    EXPECT_FALSE(failed.CanContinue());
}

TEST(MenuCaptureCompileGuard, WaitForConcurrentSaveKeepsCaptureActive) {
    using namespace std::chrono_literals;
    MenuCaptureCompileGuard guard;
    int controller, object;
    guard.ConfigureReady(true);
    guard.Begin(&controller);
    guard.Attach(&controller, &object);
    auto token = guard.StartSave(&object);
    std::thread save([&] { guard.CompleteSave(&object, token, true); });
    EXPECT_TRUE(guard.WaitForSave(&controller, 1s));
    EXPECT_FALSE(guard.CanContinue());
    save.join();
    guard.End(&controller);
    EXPECT_TRUE(guard.CanContinue());
}

TEST(MenuCaptureCompileGuard, SaveWaitTimeoutAndUnstartedFailClosed) {
    using namespace std::chrono_literals;
    int controller, object;
    MenuCaptureCompileGuard timeout;
    timeout.ConfigureReady(true);
    timeout.Begin(&controller);
    timeout.Attach(&controller, &object);
    auto token = timeout.StartSave(&object);
    EXPECT_FALSE(timeout.WaitForSave(&controller, 0ms));
    timeout.CompleteSave(&object, token, true);
    timeout.End(&controller);
    EXPECT_FALSE(timeout.CanContinue());
    MenuCaptureCompileGuard unstarted;
    unstarted.ConfigureReady(true);
    unstarted.Begin(&controller);
    unstarted.Attach(&controller, &object);
    EXPECT_FALSE(unstarted.WaitForSave(&controller, 0ms));
    EXPECT_FALSE(unstarted.CanContinue());
}
