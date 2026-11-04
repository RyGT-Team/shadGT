#include "ryty_gpu/native/ordered_pass_workers.h"
#include <cassert>
#include <thread>
#if defined(_WIN32) && defined(_MSC_VER)
#include <windows.h>
#endif

int main() {
    OrderedPassWorkers<int, int> workers(2, 2);
    std::size_t count = 0;
    workers.ForEachWorkerHandle([&](std::thread::native_handle_type handle) {
#if defined(_WIN32) && defined(_MSC_VER)
        assert(GetThreadId(handle) != 0);
        assert(GetProcessIdOfThread(handle) == GetCurrentProcessId());
#else
        (void)handle;
#endif
        ++count;
    });
    assert(count == 2);
    assert(workers.Submit(1, [](std::size_t, const int& value) { return value * 2; }));
    assert(workers.Submit(2, [](std::size_t, const int& value) { return value * 2; }));
    assert(workers.Take() == 2);
    assert(workers.Take() == 4);
    workers.Shutdown();
    count = 0;
    workers.ForEachWorkerHandle([&](std::thread::native_handle_type) { ++count; });
    assert(count == 0);
}
