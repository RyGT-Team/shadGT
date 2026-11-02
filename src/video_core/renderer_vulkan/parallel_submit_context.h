#pragma once
#include "common/types.h"
namespace Vulkan {
// Zero outside the submitting thread's active SubmitExecution scope.
u64 ActiveSubmitSignal() noexcept;
class ScopedSubmitSignal {
    u64 previous_;
public:
    explicit ScopedSubmitSignal(u64 signal) noexcept;
    ~ScopedSubmitSignal();
    ScopedSubmitSignal(const ScopedSubmitSignal&)=delete;
    ScopedSubmitSignal& operator=(const ScopedSubmitSignal&)=delete;
};
}
