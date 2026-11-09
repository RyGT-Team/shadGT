#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/libraries/kernel/threads/pthread.h"
namespace Libraries::Kernel { thread_local Pthread* g_curthread = nullptr; }
namespace Common {
std::string UTF16ToUTF8(std::wstring_view) { return {}; }
std::wstring UTF8ToUTF16W(std::string_view) { return {}; }
}
namespace Common::Log {
std::array<Level, static_cast<size_t>(Class::Count)> g_class_levels{};
void VLog(Class, Level, const char*, int, const char*, fmt::string_view, fmt::format_args) {}
}
