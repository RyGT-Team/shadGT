#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
namespace Vulkan::SceneTrace {
struct MipLayout {
    std::uint32_t width{},height{},depth{},layers{},row{},rows{};
    std::uint64_t bytes{};
};
inline MipLayout Layout(std::uint32_t width,std::uint32_t height,std::uint32_t depth,
    std::uint32_t layers,std::uint32_t mip,std::uint32_t unit_bytes,bool block) {
    MipLayout l{};
    if(!width||!height||!depth||!layers||!unit_bytes||mip>=32)return l;
    l.width=std::max(width>>mip,1u);l.height=std::max(height>>mip,1u);
    l.depth=std::max(depth>>mip,1u);l.layers=layers;
    if(block && (l.width>UINT32_MAX-3 || l.height>UINT32_MAX-3))return {};
    l.row=block?(l.width+3)&~3u:l.width;l.rows=block?(l.height+3)&~3u:l.height;
    l.bytes=unit_bytes;
    for(auto factor:{block?l.row/4:l.row,block?l.rows/4:l.rows,l.depth,l.layers}){
        if(l.bytes>std::numeric_limits<std::uint64_t>::max()/factor)return {};
        l.bytes*=factor;
    }return l;
}
}
