#include "../native/alias_probe.h"
#include <cassert>
using namespace Native::AliasProbe;
static unsigned maps=0, unmaps=0; static bool refuse=false, fail_unmap=false;
static void* Map(std::uint64_t base,std::uint64_t span) {
    ++maps; assert(base==0x4000 && span==(4ull<<20)+0x4000); return refuse ? nullptr : reinterpret_cast<void*>(0x12345000);
}
static bool Unmap(void* p,std::uint32_t& error) { assert(p==reinterpret_cast<void*>(0x12345000)); ++unmaps; if(fail_unmap) {error=42;return false;} return true; }
int main() {
    std::atomic<std::uint32_t> budget{};
    std::atomic_flag in_flight=ATOMIC_FLAG_INIT;
    auto disabled=Try(false,0x4001,4ull<<20,budget,in_flight,Map,true,Unmap); assert(disabled.status==Status::Disabled && budget==0);
    auto invalid=Try(true,~0ull,4ull<<20,budget,in_flight,Map,true,Unmap); assert(invalid.status==Status::Invalid && maps==0);
    auto missing=Try(true,0x4001,4ull<<20,budget,in_flight,Map,false,Unmap); assert(missing.status==Status::MissingExport && maps==0);
    refuse=true; auto denied=Try(true,0x4001,4ull<<20,budget,in_flight,Map,true,Unmap); assert(denied.status==Status::Refused && maps==1 && unmaps==0);
    refuse=false; auto ok=Try(true,0x4001,4ull<<20,budget,in_flight,Map,true,Unmap); assert(ok.status==Status::Success && maps==2 && unmaps==1 && ok.low_bits==0x5000);
    fail_unmap=true; auto failed=Try(true,0x4001,4ull<<20,budget,in_flight,Map,true,Unmap); assert(failed.status==Status::UnmapFailed && failed.error==42 && budget==Budget && maps==3 && unmaps==2);
    auto capped=Try(true,0x4001,4ull<<20,budget,in_flight,Map,true,Unmap); assert(capped.status==Status::BudgetExhausted && maps==3);
    in_flight.test_and_set(); auto busy=Try(true,0x4001,4ull<<20,budget,in_flight,Map,true,Unmap); assert(busy.status==Status::Busy && maps==3); in_flight.clear();
}
