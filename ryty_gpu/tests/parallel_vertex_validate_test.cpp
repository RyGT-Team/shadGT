#include <vulkan/vulkan.hpp>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
using u32=std::uint32_t;
struct Draw {std::vector<vk::VertexInputAttributeDescription2EXT> vertex_attributes;std::vector<vk::VertexInputBindingDescription2EXT> vertex_bindings;std::vector<vk::Buffer> vertex_buffers;std::vector<vk::DeviceSize> vertex_offsets,vertex_sizes,vertex_strides;u32 vertex_attribute_count{},vertex_binding_count{},vertex_buffer_count{};bool vertex_backing_verified=true;};
bool Validate(const Draw& draw){
#include "vertex-validate-under-test.inc"
return true;}
#define CHECK(x) do{if(!(x)){std::printf("FAIL %d\n",__LINE__);std::abort();}}while(0)
Draw make(unsigned n){Draw d;d.vertex_attribute_count=d.vertex_binding_count=d.vertex_buffer_count=n;for(unsigned i=0;i<n&&i<32;i++){vk::VertexInputBindingDescription2EXT b{};b.binding=i;vk::VertexInputAttributeDescription2EXT a{};a.binding=i;d.vertex_bindings.push_back(b);d.vertex_attributes.push_back(a);d.vertex_buffers.push_back(vk::Buffer{reinterpret_cast<VkBuffer>(uintptr_t(i+1))});d.vertex_offsets.push_back(0);d.vertex_sizes.push_back(64);d.vertex_strides.push_back(16);}return d;}
int main(){for(unsigned n:{0u,1u,4u,12u,32u}){auto p=make(n);CHECK(Validate(p));if(n)for(unsigned i:{0u,n/2,n-1})for(unsigned k=0;k<6;k++){auto q=p;switch(k){case 0:q.vertex_bindings[i].binding++;break;case 1:q.vertex_attributes[i].binding++;break;case 2:q.vertex_buffers[i]=nullptr;break;case 3:q.vertex_sizes[i]=0;break;case 4:q.vertex_attributes[i].pNext=reinterpret_cast<void*>(1);break;case 5:q.vertex_bindings[i].pNext=reinterpret_cast<void*>(1);break;}CHECK(!Validate(q));}if(n){auto q=p;q.vertex_offsets.pop_back();CHECK(!Validate(q));q=p;q.vertex_binding_count--;CHECK(!Validate(q));q=p;q.vertex_backing_verified=false;CHECK(!Validate(q));}}{auto q=make(4);q.vertex_attributes[0].binding++;q.vertex_buffers[3]=nullptr;CHECK(!Validate(q));}
    for(unsigned k=0;k<6;k++){auto q=make(4);switch(k){case 0:q.vertex_attributes.pop_back();break;case 1:q.vertex_bindings.pop_back();break;case 2:q.vertex_buffers.pop_back();break;case 3:q.vertex_offsets.pop_back();break;case 4:q.vertex_sizes.pop_back();break;case 5:q.vertex_strides.pop_back();break;}CHECK(!Validate(q));}
    CHECK(!Validate(make(33)));std::puts("PASS extracted vertex validation");}
