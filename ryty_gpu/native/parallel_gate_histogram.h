#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Fixed recorder-local diagnostic storage. A slot with count=0 is empty; mask
// zero is a valid key. No allocation and at most eight probes per observation.
struct ParallelGateHistogram {
    using Word=std::uint64_t;
    struct Slot {Word mask{},count{};};
    std::array<Slot,128> slots{};
    Word observed{},unstable{},overflow{};
    void Add(Word mask,bool stable) noexcept {
        ++observed;if(!stable){++unstable;return;}
        const std::size_t start=(mask*Word{0x9E3779B97F4A7C15ull})>>57;
        for(std::size_t i=0;i<8;++i) {
            auto& slot=slots[(start+i)&127u];
            if(!slot.count){slot.mask=mask;slot.count=1;return;}
            if(slot.mask==mask){++slot.count;return;}
        }
        ++overflow;
    }
    std::array<Slot,16> Top() const noexcept {
        std::array<Slot,16> result{};
        for(const auto& slot:slots) if(slot.count) {
            for(std::size_t i=0;i<result.size();++i) if(slot.count>result[i].count) {
                for(std::size_t j=result.size()-1;j>i;--j)result[j]=result[j-1];
                result[i]=slot;break;
            }
        }
        return result;
    }
};
