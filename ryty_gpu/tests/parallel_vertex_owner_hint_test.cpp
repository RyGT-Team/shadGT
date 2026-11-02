#include <cassert>
#include <cstdint>
#include <memory>
#include <limits>
#include <vector>
#include "ryty_gpu/native/prepared_index_bounds.h"

using u64 = std::uint64_t;
struct VkBuffer_T {};
template<class T> T* Handle(std::uintptr_t value) { return reinterpret_cast<T*>(value); }
namespace vk { using Buffer = VkBuffer_T*; }
struct Buffer {
    std::uintptr_t handle;
    u64 capacity;
    std::uintptr_t NativeHandle() const { return handle; }
    u64 Size() const { return capacity; }
};
struct Region { std::unique_ptr<Buffer> buffer; u64 begin{},end{}; };
struct Mirror { std::unique_ptr<Buffer> buffer; u64 begin{},end{}; };
struct Use { Region* region; };
struct Prepared { std::vector<Use> regions; std::vector<Mirror*> mirrors; bool native_vertex_receipt{},native_index_receipt{}; };
struct Vertex { std::vector<vk::Buffer> buffers; std::vector<u64> offsets, sizes; };

static std::vector<bool> checked(Prepared* prepared, Vertex* vi) {
    bool vertex_safe = true;
    std::vector<bool> results;
#include "vertex-owner-hint-under-test.inc"
                results.push_back(owned);
            }
    return results;
}

template<class Owner> bool Constructed(const Owner& owner,u64 address,u64 size) {
#include "vertex-native-receipt-under-test.inc"
    return valid_native(owner);
}
static bool SelectVertex(Prepared* prepared,Vertex* vi,bool vertex_safe,bool known_shape,bool native_receipt_valid,size_t& skipped) {
    bool native_vi_receipt_used=false;size_t native_vertex_skipped=0;
#include "vertex-receipt-select.inc"
    skipped=native_vertex_skipped;assert(native_vi_receipt_used==native_vertex);return native_vertex;
}
static bool SelectIndex(Prepared* prepared,bool native_receipt_valid) {
    bool index_safe=false,native_vi_receipt_used=false;
#include "index-receipt-select.inc"
    assert(index_safe==native_vi_receipt_used);return index_safe;
}
// Model the old native scan plus direct/coherence fallback after the extracted selector.
static bool Admit(Prepared* p,Vertex* v,bool safe,bool shape,bool context,bool direct_coherent,int& direct_calls) {
    size_t skipped=0;if(SelectVertex(p,v,safe,shape,context,skipped))return true;
    if(!safe || !shape)return false;
    const auto old=checked(p,v);for(bool owned:old)if(!owned){++direct_calls;if(!direct_coherent)return false;}
    return true;
}
int main() {
    { // Real producer predicate: handle, interval and native backing must agree.
     Region owner{std::make_unique<Buffer>(Buffer{1,32}),100,132};
     assert(Constructed(owner,100,32));assert(Constructed(owner,116,16));
     assert(!Constructed(owner,99,1) && !Constructed(owner,132,1) && !Constructed(owner,100,0));
     owner.buffer->capacity=20;assert(!Constructed(owner,116,16));owner.buffer->capacity=32;
     owner.buffer->handle=0;assert(!Constructed(owner,100,1));owner.buffer.reset();assert(!Constructed(owner,100,1));
     owner.buffer=std::make_unique<Buffer>(Buffer{1,32});u64 bytes=0;
     assert(Native::Parallel::PreparedIndexRange(100,2,4,2,104,4,bytes) && bytes==8);
     assert(Constructed(owner,104,bytes) && Native::Parallel::PreparedIndexBacking(104,bytes,owner.begin,owner.end,4,owner.buffer->Size()));
     assert(!Native::Parallel::PreparedIndexBacking(104,bytes,owner.begin,owner.end,5,owner.buffer->Size()));
    }
    {
     Region owner{std::make_unique<Buffer>(Buffer{1,32}),100,132};Prepared p{{{&owner}},{},true,true};
     Vertex v{{Handle<VkBuffer_T>(1)},{4},{8}};size_t skipped=0;
     assert(SelectVertex(&p,&v,true,true,true,skipped) && skipped==1 && checked(&p,&v)[0]);
     for(bool safe:{false,true})for(bool shape:{false,true})for(bool context:{false,true}) {
      const bool fast=SelectVertex(&p,&v,safe,shape,context,skipped);
      assert(fast==(safe && shape && context));if(!fast)assert(checked(&p,&v)[0]);
     }
     p.native_vertex_receipt=false;assert(!SelectVertex(&p,&v,true,true,true,skipped)); // mixed/import uses old gate
     u64 bytes=0;assert(Native::Parallel::PreparedIndexRange(100,2,4,2,104,4,bytes));
     assert(SelectIndex(&p,true) && Native::Parallel::PreparedIndexBacking(104,bytes,100,132,4,32));
     // One imported vertex invalidates aggregate VI even if the index remains native.
     v.buffers.push_back(Handle<VkBuffer_T>(2));v.offsets.push_back(0);v.sizes.push_back(8);int direct_calls=0;
     assert(SelectIndex(&p,true));assert(Admit(&p,&v,true,true,true,true,direct_calls) && direct_calls==1);
     direct_calls=0;assert(!Admit(&p,&v,true,true,false,false,direct_calls) && direct_calls==1);
     direct_calls=0;assert(!Admit(&p,&v,true,false,true,true,direct_calls) && direct_calls==0);
     Vertex empty;assert(!SelectVertex(&p,&empty,true,false,true,skipped));
     p.native_index_receipt=false;assert(!SelectIndex(&p,true));assert(!SelectIndex(&p,false));
     assert(!Native::Parallel::PreparedIndexBacking(104,bytes,100,132,5,32));
     assert(!Native::Parallel::PreparedIndexBacking(104,bytes,100,132,4,11));
    }
    Region first{std::make_unique<Buffer>(Buffer{1, 16})};
    Region second{std::make_unique<Buffer>(Buffer{1, 32})};
    Region missing{};
    Mirror mirror{std::make_unique<Buffer>(Buffer{2, 16})};
    Prepared prepared{{{&missing}, {&first}, {&second}}, {&mirror}};
    Vertex vi{{Handle<VkBuffer_T>(1), Handle<VkBuffer_T>(1), Handle<VkBuffer_T>(2),
               Handle<VkBuffer_T>(2), nullptr, Handle<VkBuffer_T>(3)},
              {0, 20, 1, 15, 0, 0}, {8, 8, 8, 2, 1, 1}};
    // Second binding must fall back from the first owner to the larger one.
    // Fourth exceeds the mirror, and null and unknown handles cannot pass.
    assert((checked(&prepared, &vi) == std::vector<bool>{true, true, true, false, false, false}));
    first.buffer.reset();
    vi.buffers.resize(2); vi.offsets.resize(2); vi.sizes.resize(2);
    assert((checked(&prepared, &vi) == std::vector<bool>{true, true}));
    second.buffer.reset();
    assert((checked(&prepared, &vi) == std::vector<bool>{false, false}));

    first.buffer = std::make_unique<Buffer>(Buffer{1, 16});
    const u64 max = std::numeric_limits<u64>::max();
    vi = {{Handle<VkBuffer_T>(1), Handle<VkBuffer_T>(1), Handle<VkBuffer_T>(1),
           Handle<VkBuffer_T>(1), Handle<VkBuffer_T>(1), Handle<VkBuffer_T>(2),
           Handle<VkBuffer_T>(1)},
          {0, 16, 17, max, 1, 0, 0}, {1, 0, 1, 1, max, 1, 1}};
    // Every binding is rechecked, including subtraction boundaries. Switching
    // handles must find the mirror and then return to the region.
    assert((checked(&prepared, &vi) ==
            std::vector<bool>{true, false, false, false, false, true, true}));
}
