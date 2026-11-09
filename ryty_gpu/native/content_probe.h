#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
namespace Native::ContentProbe {
struct Scratch { std::unique_ptr<std::uint8_t[]> bytes; std::size_t capacity=0; bool busy=false; };
enum class Status { Unavailable, CopyFailed, Valid };
struct Result { Status status=Status::Unavailable; std::uint64_t hash=0; };
// The snapshot is consumed synchronously; neither it nor the scratch pointer reaches the GPU.
template<class Copy, class Hash, class Forward>
Result Try(Scratch& scratch, bool enabled, std::size_t size, std::size_t readable,
           const void* source, void* destination, Copy copy, Hash hash, Forward forward) {
    if (!enabled || size < (4ull<<20) || size > (64ull<<20) || readable != size || scratch.busy)
        return {};
    struct Busy { Scratch& s; explicit Busy(Scratch& v):s(v){s.busy=true;} ~Busy(){s.busy=false;} } busy{scratch};
    if (scratch.capacity < size) {
        scratch.bytes.reset(); scratch.capacity=0;
        try { scratch.bytes.reset(new std::uint8_t[size]); scratch.capacity=size; }
        catch (const std::bad_alloc&) { return {}; }
    }
    if (!copy(scratch.bytes.get(),source,size)) return {Status::CopyFailed,0};
    const auto value=hash(scratch.bytes.get(),size);
    forward(destination,scratch.bytes.get(),size);
    return {Status::Valid,value};
}
}
