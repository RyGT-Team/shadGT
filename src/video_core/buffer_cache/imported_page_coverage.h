// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <bit>
#include <cstdint>
#include <memory>
#include <optional>

namespace VideoCore {
// Publication records completed imports, never pointers into the mutable view map.
// Writers serialize externally. Invalidation remains odd until all affected views
// and GPU page-table entries have been removed. Readers accept one stable epoch.
template<unsigned AddressBits=40, unsigned PageBits=14>
class ImportedPageCoverage {
    static_assert(AddressBits<64 && AddressBits>=PageBits+6);
    using U=std::uint64_t;
    static_assert(std::atomic<U>::is_always_lock_free);
public:
    static constexpr U Limit=U{1}<<AddressBits;
    static constexpr U PageSize=U{1}<<PageBits;
    static constexpr U WordCount=U{1}<<(AddressBits-PageBits-6);
    static constexpr U StorageBytes=WordCount*sizeof(U);
    ImportedPageCoverage():words{std::make_unique<std::atomic<U>[]>(WordCount)}{}
    class Invalidation {
    public:
        explicit Invalidation(ImportedPageCoverage& owner):owner{owner} {
            const auto old=owner.epoch.fetch_add(1,std::memory_order_acq_rel);
            assert((old&1)==0);
        }
        ~Invalidation(){owner.epoch.fetch_add(1,std::memory_order_release);}
        Invalidation(const Invalidation&)=delete;
        Invalidation& operator=(const Invalidation&)=delete;
    private: ImportedPageCoverage& owner;
    };
    [[nodiscard]] Invalidation Invalidate(){return Invalidation{*this};}
    void Publish(U begin,U end){Change<true>(begin,end);}
    void Clear(U begin,U end){Change<false>(begin,end);}
    bool Covers(U address,U size) const {
        if(address>Limit || size>Limit-address)return false;
        if(size==0)return true;
        const auto before=epoch.load(std::memory_order_acquire);
        if(before&1)return false;
        const U first=address>>PageBits,last=(address+size-1)>>PageBits;
        for(U word=first>>6;word<=last>>6;++word){
            const U mask=Mask(word,first,last);
            if((words[word].load(std::memory_order_acquire)&mask)!=mask)return false;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        return epoch.load(std::memory_order_relaxed)==before;
    }
    // The importer holds its view mutex when using this to skip a completed
    // prefix. A missing suffix must still take the original import/failure path.
    std::optional<U> FirstMissing(U address,U size) const {
        if(address>Limit || size>Limit-address)return std::nullopt;
        if(size==0)return address;
        const auto before=epoch.load(std::memory_order_acquire);
        if(before&1)return std::nullopt;
        const U first=address>>PageBits,last=(address+size-1)>>PageBits;
        U result=address+size;
        for(U word=first>>6;word<=last>>6;++word){
            const U missing=~words[word].load(std::memory_order_acquire)&Mask(word,first,last);
            if(missing){
                result=std::max(address,((word<<6)+std::countr_zero(missing))<<PageBits);
                break;
            }
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if(epoch.load(std::memory_order_relaxed)!=before)return std::nullopt;
        return result;
    }
private:
    static U Mask(U word,U first,U last){
        U mask=~U{0};
        if(word==(first>>6))mask&=~U{0}<<(first&63);
        if(word==(last>>6))mask&=~U{0}>>(63-(last&63));
        return mask;
    }
    template<bool Add> void Change(U begin,U end){
        assert(begin<=end && end<=Limit && ((begin|end)&(PageSize-1))==0);
        if(begin>=end)return;
        const U first=begin>>PageBits,last=(end-1)>>PageBits;
        for(U word=first>>6;word<=last>>6;++word){
            const U mask=Mask(word,first,last);
            if constexpr(Add)words[word].fetch_or(mask,std::memory_order_release);
            else words[word].fetch_and(~mask,std::memory_order_release);
        }
    }
    std::unique_ptr<std::atomic<U>[]> words;
    std::atomic<U> epoch{};
};
}
