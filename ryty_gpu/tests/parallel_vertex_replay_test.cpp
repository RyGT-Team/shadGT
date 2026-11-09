#include <vulkan/vulkan.hpp>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "video_core/renderer_vulkan/vertex_input_equal.h"
struct Draw {vk::Pipeline pipeline{};std::vector<vk::VertexInputAttributeDescription2EXT> vertex_attributes;std::vector<vk::VertexInputBindingDescription2EXT> vertex_bindings;std::vector<vk::Buffer> vertex_buffers;std::vector<vk::DeviceSize> vertex_offsets,vertex_sizes,vertex_strides;uint32_t vertex_binding_count{},vertex_attribute_count{},vertex_buffer_count{};};
struct Instance{bool dynamic;bool IsVertexInputDynamicState() const{return dynamic;}};
struct Commands{unsigned state{},bind1{},bind2{};void setVertexInputEXT(vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT>,vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT>){++state;}void bindVertexBuffers(uint32_t,uint32_t,const vk::Buffer*,const vk::DeviceSize*){++bind1;}void bindVertexBuffers2(uint32_t,uint32_t,const vk::Buffer*,const vk::DeviceSize*,const vk::DeviceSize*,const vk::DeviceSize*){++bind2;}};
Commands record(const std::vector<Draw>& rows,bool mode){Instance instance{mode};Commands commands;const Draw* bound_vertex=nullptr;
#include "vertex-same-under-test.inc"
for(const auto& draw:rows){
#include "vertex-record-under-test.inc"
}return commands;}
#define CHECK(x) do{if(!(x)){std::printf("FAIL %d\n",__LINE__);std::abort();}}while(0)
Draw make(){Draw d;d.pipeline=vk::Pipeline{reinterpret_cast<VkPipeline>(uintptr_t(1))};d.vertex_attributes.resize(1);d.vertex_bindings.resize(1);d.vertex_buffers={vk::Buffer{reinterpret_cast<VkBuffer>(uintptr_t(1))}};d.vertex_offsets={0};d.vertex_sizes={64};d.vertex_strides={16};d.vertex_binding_count=d.vertex_attribute_count=d.vertex_buffer_count=1;return d;}
int main(){Draw a=make(),b=a;for(bool dynamic:{false,true}){auto r=record({a,b},dynamic);CHECK(dynamic?(r.state==1&&r.bind1==1):(r.bind2==1));auto again=record({a},dynamic);CHECK(dynamic?again.state==1:again.bind2==1);b.pipeline=vk::Pipeline{reinterpret_cast<VkPipeline>(uintptr_t(2))};r=record({a,b},dynamic);CHECK(dynamic?r.state==2:r.bind2==2);b=a;b.vertex_buffers[0]=vk::Buffer{reinterpret_cast<VkBuffer>(uintptr_t(2))};r=record({a,b},dynamic);CHECK(dynamic?r.state==2:r.bind2==2);b=a;b.vertex_offsets[0]++;r=record({a,b},dynamic);CHECK(dynamic?r.state==2:r.bind2==2);b=a;b.vertex_sizes[0]++;r=record({a,b},dynamic);CHECK(dynamic?r.state==2:r.bind2==2);b=a;b.vertex_strides[0]++;r=record({a,b},dynamic);CHECK(dynamic?r.state==2:r.bind2==2);b=a;b.vertex_attributes[0].format=vk::Format::eR32Sfloat;r=record({a,b},dynamic);CHECK(dynamic?r.state==2:r.bind2==2);b=a;b.vertex_bindings[0].divisor++;r=record({a,b},dynamic);CHECK(dynamic?r.state==2:r.bind2==2);b=a;}
Draw z;z.pipeline=a.pipeline;auto dz=record({z,z},true);auto sz=record({z,z},false);CHECK(dz.state==1&&dz.bind1==0&&sz.bind2==0);std::puts("PASS extracted emission branch, first/new-pass and mutations in dynamic/static modes");}
