// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vertex_input_equal.h"
#include <vulkan/vulkan.hpp>
#include <vector>
#include <array>
#include <stdexcept>
#include <iostream>
template<class T> void Check(const std::vector<T>& a, const std::vector<T>& b) {
    if(Vulkan::VertexInputBytesEqual(a,b) && a!=b)throw std::runtime_error("false equality");
}
int main() {
    unsigned cases=0;
    for(unsigned count=0;count<=32;++count) {
        std::vector<vk::VertexInputBindingDescription2EXT> bindings(count);
        std::vector<vk::VertexInputAttributeDescription2EXT> attrs(count);
        if(!Vulkan::VertexInputBytesEqual(bindings,bindings)||!Vulkan::VertexInputBytesEqual(attrs,attrs))throw std::runtime_error("self equality failed");
        for(unsigned i=0;i<count;++i) {
            for(unsigned field=0;field<6;++field) {
                auto b=bindings;auto a=attrs;
                switch(field){
                case 0:b[i].binding=7;a[i].binding=7;break;
                case 1:b[i].stride=17;a[i].offset=17;break;
                case 2:b[i].inputRate=vk::VertexInputRate::eInstance;a[i].format=vk::Format::eR32Sfloat;break;
                case 3:b[i].divisor=19;a[i].location=19;break;
                case 4:b[i].pNext=&cases;a[i].pNext=&cases;break;
                case 5:b.push_back({});a.push_back({});break;
                }
                Check(bindings,b);Check(attrs,a);
                if(Vulkan::VertexInputBytesEqual(bindings,b)||Vulkan::VertexInputBytesEqual(attrs,a))throw std::runtime_error("changed state missed");
                cases+=2;
            }
        }
    }
    std::cout<<"PASS "<<cases<<" Vulkan vertex-state mutation cases\n";
}
