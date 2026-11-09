#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <span>
#include <utility>

namespace Native::Parallel {
// Opt-in observation only; import selection and teardown remain unchanged.
// Overflow poisons the owner instead of affecting normal imports.
template<std::size_t OwnerCount=8, std::size_t ViewCount=4096>
class DirectCoherenceRegistry {
    struct Owner { const void* key{}; bool tables{}; bool poisoned{}; };
    struct View { const void* owner{}; std::uint64_t begin{}, end{}; bool coherent{}; };
    mutable std::mutex mutex_;
    std::array<Owner,OwnerCount> owners_{};
    std::array<View,ViewCount> views_{};
    // Live coherent view slots sorted by (owner address, begin, slot). Prefix
    // maxima reset at each owner boundary. Both are protected by mutex_.
    std::array<std::size_t,ViewCount> index_{};
    std::array<std::uint64_t,ViewCount> prefix_end_{};
    std::size_t index_size_{};
    static std::uintptr_t Key(const void* p) noexcept {return reinterpret_cast<std::uintptr_t>(p);}
    bool Less(std::size_t a,std::size_t b) const noexcept {
        const auto& x=views_[a];const auto& y=views_[b];
        if(Key(x.owner)!=Key(y.owner))return Key(x.owner)<Key(y.owner);
        if(x.begin!=y.begin)return x.begin<y.begin;
        return a<b;
    }
    void RebuildPrefixes() noexcept {
        std::uint64_t max_end=0;const void* previous=nullptr;
        for(std::size_t i=0;i<index_size_;++i) {
            const auto& v=views_[index_[i]];
            if(i==0 || v.owner!=previous)max_end=v.end;
            else if(v.end>max_end)max_end=v.end;
            prefix_end_[i]=max_end;previous=v.owner;
        }
    }
    void RemoveSlot(std::size_t slot) noexcept {
        for(std::size_t i=0;i<index_size_;++i)if(index_[i]==slot){
            for(std::size_t j=i+1;j<index_size_;++j)index_[j-1]=index_[j];--index_size_;return;
        }
    }
    void InsertSlot(std::size_t slot) noexcept {
        std::size_t lo=0,hi=index_size_;
        while(lo<hi){const auto mid=lo+(hi-lo)/2;if(Less(index_[mid],slot))lo=mid+1;else hi=mid;}
        for(std::size_t i=index_size_;i>lo;--i)index_[i]=index_[i-1];
        index_[lo]=slot;++index_size_;
    }
    void Compact() noexcept {
        std::size_t n=0;for(std::size_t i=0;i<index_size_;++i)if(views_[index_[i]].coherent)index_[n++]=index_[i];
        index_size_=n;RebuildPrefixes();
    }
    Owner* Find(const void* key) {for(auto& o:owners_) if(o.key==key) return &o; return nullptr;}
public:
#ifdef RYGT_COHERENCE_TEST
    template<class F> void TestWithLock(F&& f) {std::scoped_lock lock(mutex_);f();}
#endif
    void Register(const void* key,bool tables) noexcept {std::scoped_lock l(mutex_);for(auto& o:owners_)if(!o.key){o={key,tables,false};return;}}
    void Erase(const void* key) noexcept {std::scoped_lock l(mutex_);for(auto& v:views_)if(v.owner==key)v={};Compact();if(auto* o=Find(key))*o={};}
    void Add(const void* key,std::uint64_t begin,std::uint64_t end,bool coherent) noexcept {
        std::scoped_lock l(mutex_);auto* o=Find(key);if(!o || begin>=end)return;
        for(const auto& v:views_)if(v.owner==key && v.begin<end && begin<v.end){o->poisoned=true;return;}
        for(std::size_t i=0;i<ViewCount;++i)if(!views_[i].owner){
            // A null-owned coherent view is also considered free by the original
            // registry; remove its old key before overwriting its fields.
            if(views_[i].coherent)RemoveSlot(i);
            views_[i]={key,begin,end,coherent};if(coherent)InsertSlot(i);RebuildPrefixes();return;
        }
        o->poisoned=true;
    }
    void Remove(const void* key,std::uint64_t begin,std::uint64_t end) noexcept {
        std::scoped_lock l(mutex_);for(auto& v:views_)if(v.owner==key && v.begin<end && begin<v.end)v={};Compact();
    }
private:
    bool CoversLocked(const void* key,std::uint64_t begin,std::uint64_t size) const noexcept {
        if(!size || begin>UINT64_MAX-size)return false;
        const Owner* o=nullptr;for(const auto& x:owners_)if(x.key==key){o=&x;break;}
        if(!o || !o->tables || o->poisoned)return false;
        const auto end=begin+size;auto cursor=begin;const auto owner=Key(key);
        while(cursor<end){
            std::size_t lo=0,hi=index_size_;
            while(lo<hi){const auto mid=lo+(hi-lo)/2;const auto& v=views_[index_[mid]];
                if(Key(v.owner)<owner || (Key(v.owner)==owner && v.begin<=cursor))lo=mid+1;else hi=mid;}
            if(!lo)return false;const auto pos=lo-1;const auto& v=views_[index_[pos]];
            if(v.owner!=key || prefix_end_[pos]<=cursor)return false;
            cursor=prefix_end_[pos];
        }
        return true;
    }
public:
    bool Covers(const void* key,std::uint64_t begin,std::uint64_t size) const noexcept {
        if(!size || begin>UINT64_MAX-size)return false;
        std::unique_lock l(mutex_,std::try_to_lock);return l.owns_lock() && CoversLocked(key,begin,size);
    }
    // Empty input is the conjunction of zero checks; nonempty input takes one try-lock.
    bool CoversAll(const void* key,std::span<const std::pair<std::uint64_t,std::uint64_t>> ranges) const noexcept {
        if(ranges.empty())return true;
        std::unique_lock l(mutex_,std::try_to_lock);if(!l.owns_lock())return false;
        for(const auto& [begin,end]:ranges)if(end<=begin || !CoversLocked(key,begin,end-begin))return false;
        return true;
    }
};
}
