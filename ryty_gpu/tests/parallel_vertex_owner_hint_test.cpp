#include <cassert>
#include <cstdint>
#include <memory>
#include <limits>
#include <vector>

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
struct Region { std::unique_ptr<Buffer> buffer; };
struct Mirror { std::unique_ptr<Buffer> buffer; };
struct Use { Region* region; };
struct Prepared { std::vector<Use> regions; std::vector<Mirror*> mirrors; };
struct Vertex { std::vector<vk::Buffer> buffers; std::vector<u64> offsets, sizes; };

static std::vector<bool> checked(Prepared* prepared, Vertex* vi) {
    bool vertex_safe = true;
    std::vector<bool> results;
#include "vertex-owner-hint-under-test.inc"
                results.push_back(owned);
            }
    return results;
}

static std::vector<bool> Reference(Prepared* prepared,Vertex* vi) {
    std::vector<bool> results;
    for(size_t i=0;i<vi->buffers.size();++i) {
        const auto handle=vi->buffers[i];const auto offset=vi->offsets[i],size=vi->sizes[i];
        const auto matches=[&](const auto* value) {
            if(!value || !value->buffer || !handle || !size)return false;
            if(Handle<VkBuffer_T>(value->buffer->NativeHandle())!=handle)return false;
            const u64 capacity=value->buffer->Size();return offset<=capacity && size<=capacity-offset;
        };
        bool found=false;for(const auto& region:prepared->regions)if(matches(region.region)){found=true;break;}
        if(!found)for(const auto* mirror:prepared->mirrors)if(matches(mirror)){found=true;break;}
        results.push_back(found);
    }
    return results;
}
int main() {
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
    assert(checked(&prepared,&vi)==Reference(&prepared,&vi));
    std::swap(prepared.regions[1],prepared.regions[2]);assert(checked(&prepared,&vi)==Reference(&prepared,&vi));
    prepared.regions.erase(prepared.regions.begin()+1);assert(checked(&prepared,&vi)==Reference(&prepared,&vi));
    prepared.regions.insert(prepared.regions.begin()+1,{&second});
    second.buffer.reset();
    assert((checked(&prepared, &vi) == std::vector<bool>{false, false}));

    first.buffer = std::make_unique<Buffer>(Buffer{1, 16});
    const u64 max = std::numeric_limits<u64>::max();
    vi = {{Handle<VkBuffer_T>(1), Handle<VkBuffer_T>(1), Handle<VkBuffer_T>(1),
           Handle<VkBuffer_T>(1), Handle<VkBuffer_T>(1), Handle<VkBuffer_T>(2),
           Handle<VkBuffer_T>(1)},
          {0, 16, 17, max, 1, 0, 0}, {1, 0, 1, 1, max, 1, 1}};
    assert(checked(&prepared,&vi)==Reference(&prepared,&vi));
    // Every binding is rechecked, including subtraction boundaries. Switching
    // handles must find the mirror and then return to the region.
    assert((checked(&prepared, &vi) ==
            std::vector<bool>{true, false, false, false, false, true, true}));
}
