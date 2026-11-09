#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace Native::Parallel {
// A positive-only memo for objects owned for the entire enclosing sidecar
// lifetime. Caller must clear/destroy it before destroying the owned objects.
template<class Handle,std::size_t N=256> class PositiveHandleCache {
    static_assert(N && !(N&(N-1)));
    std::array<Handle,N> entries_{};
    static std::size_t Slot(std::uint64_t key) noexcept {
        key^=key>>33;key*=0xff51afd7ed558ccdULL;key^=key>>33;
        return static_cast<std::size_t>(key)&(N-1);
    }
public:
    bool Contains(Handle handle,std::uint64_t key) const noexcept {return key && bool(handle) && entries_[Slot(key)]==handle;}
    void Insert(Handle handle,std::uint64_t key) noexcept {if(key && bool(handle))entries_[Slot(key)]=handle;}
    void Clear() noexcept {entries_.fill(Handle{});}
};
}
