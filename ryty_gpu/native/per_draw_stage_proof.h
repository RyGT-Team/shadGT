#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
namespace Native::Parallel {
// Stack-local to one committed draw. No pointer survives the producer call;
// view/coherence checks remain live and are deliberately not cached here.
template<class Stage,std::size_t N> class PerDrawStageProof {
public:
    struct Entry {const Stage* stage{};std::size_t begin{},count{};unsigned table_state{};const std::byte* data{};std::size_t table_size{};std::uint64_t l1{},zero{};};
private:
    std::array<Entry,N> entries_{};std::size_t size_{};bool initialized_{},valid_{};std::uint32_t builds_{},hits_{};
public:
    std::uint32_t Builds() const noexcept {return builds_;}
    std::uint32_t Hits() const noexcept {return hits_;}
    template<class Stages> const Entry* Find(const Stages& stages,std::size_t index) noexcept {
        if(!initialized_) {
            initialized_=true;valid_=true;std::size_t begin=0;
            for(std::size_t i=0;i<stages.size() && i<N;++i) {
                const auto* stage=stages[i];if(!stage)continue;const auto count=stage->buffers.size();
                if(count>std::numeric_limits<std::size_t>::max()-begin){valid_=false;break;}
                entries_[size_++]={stage,begin,count};begin+=count;
            }
        }
        if(!valid_)return nullptr;
        for(std::size_t i=0;i<size_;++i)if(index>=entries_[i].begin && index-entries_[i].begin<entries_[i].count)return &entries_[i];
        return nullptr;
    }
    template<class Uploads,class Bytes,class IsTable>
    std::span<const std::byte> Table(const Entry* found,const Uploads& uploads,const Bytes& bytes,
                                     std::uint64_t expected_l1,std::uint64_t expected_zero,IsTable is_table) noexcept {
        if(!found)return {};
        auto* e=const_cast<Entry*>(found);
        if(e->table_state && (e->l1!=expected_l1 || e->zero!=expected_zero))e->table_state=0;
        if(e->table_state)++hits_;
        if(e->table_state==0) {
            ++builds_;e->l1=expected_l1;e->zero=expected_zero;
            e->table_state=2;std::size_t table=e->count;
            for(std::size_t j=0;j<e->count;++j)if(is_table(e->stage->buffers[j])){if(table!=e->count)return {};table=j;}
            if(table==e->count || e->count>(std::numeric_limits<std::size_t>::max()-16)/16)return {};
            const auto expected=16+e->count*16,global=e->begin+table;
            const typename Uploads::value_type* selected=nullptr;
            for(const auto& upload:uploads)if(upload.info==global){if(selected)return {};selected=&upload;}
            if(!selected || selected->flat_stage>=0 || selected->size!=expected || selected->offset>bytes.size() || expected>bytes.size()-selected->offset)return {};
            const auto* data=reinterpret_cast<const std::byte*>(bytes.data()+selected->offset);
            std::uint32_t h[4]{};std::memcpy(h,data,sizeof(h));
            const std::uint64_t l1=std::uint64_t(h[0])|(std::uint64_t(h[1])<<32),zero=std::uint64_t(h[2])|(std::uint64_t(h[3])<<32);
            if(l1!=expected_l1 || zero!=expected_zero)return {};
            e->data=data;e->table_size=expected;e->l1=l1;e->zero=zero;e->table_state=1;
        }
        return e->table_state==1 && e->l1==expected_l1 && e->zero==expected_zero?std::span<const std::byte>{e->data,e->table_size}:std::span<const std::byte>{};
    }
};
}
