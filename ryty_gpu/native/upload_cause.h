#pragma once
#include <cstdint>
namespace Native::UploadCause {
struct CpuFault { std::uint64_t address{}, size{}, exact{}; bool active{}; };
inline thread_local CpuFault cpu_fault{};
class ScopedCpuFault {
public:
    ScopedCpuFault(std::uint64_t address, std::uint64_t size, std::uint64_t exact)
        : previous{cpu_fault} { cpu_fault = {address,size,exact,true}; }
    ~ScopedCpuFault() { cpu_fault = previous; }
private:
    CpuFault previous;
};
}
