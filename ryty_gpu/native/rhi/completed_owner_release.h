#pragma once
#include <functional>
namespace Native::Rhi {class Device; bool TryEnqueueCompleted(Device&,std::function<void()>&) noexcept;}