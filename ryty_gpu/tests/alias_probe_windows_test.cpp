#include "../native/alias_probe_windows.h"
#include <cassert>
int main() {
    auto module=GetModuleHandleW(L"KernelBase.dll"); assert(module);
    auto unmap=reinterpret_cast<Native::AliasProbe::UnmapFn>(GetProcAddress(module,"UnmapViewOfFile2")); assert(unmap);
    HANDLE section=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,128*1024,nullptr); assert(section);
    void* base=MapViewOfFile(section,FILE_MAP_ALL_ACCESS,0,0,128*1024); assert(base);
    void* alias=static_cast<char*>(base)+0x4000;
    std::uint32_t error=0; assert(Native::AliasProbe::Cleanup(alias,error,unmap));
    MEMORY_BASIC_INFORMATION info{}; assert(VirtualQuery(base,&info,sizeof(info))==sizeof(info));
    assert(info.State==MEM_FREE); assert(CloseHandle(section));
}
