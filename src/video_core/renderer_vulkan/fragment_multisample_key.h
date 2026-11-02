#pragma once
#include <cstdint>
namespace Vulkan {
// GPL links with a non-null fragment pMultisampleState require an identical
// fragment-output state (VUID-VkGraphicsPipelineCreateInfo-pLibraries-06635),
// including when sample shading is disabled. Add fields individually: no padding
// or host pointer addresses participate in the cache identity.
template<class Key, class State>
void AddFragmentMultisampleKey(Key& key, const State& state) {
    key.Add(state.flags);
    key.Add(state.rasterizationSamples);
    key.Add(state.sampleShadingEnable);
    key.Add(state.minSampleShading);
    key.Add(state.alphaToCoverageEnable);
    key.Add(state.alphaToOneEnable);
    const bool explicit_mask = state.pSampleMask != nullptr;
    key.Add(explicit_mask);
    if (explicit_mask) {
        const auto words = (static_cast<std::uint32_t>(state.rasterizationSamples) + 31u) / 32u;
        for (std::uint32_t i=0;i<words;++i) key.Add(state.pSampleMask[i]);
    }
}
}
