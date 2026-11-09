#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace Native::Parallel {
struct ViewProofStats {std::uint64_t queries{},first_hits{},index_hits{},walks{},fallbacks{},overflows{},wanted_builds{},wanted_overflows{},filtered_views{};};
// One instance per descriptor-proof loop. Only verified positives are retained;
// every unproved query falls back to the original full scan. Walk advances its
// cursor only after a complete sampled-entry visit under views_mutex.
template<class Handle,std::size_t Capacity=128> class PerDrawViewProof {
    static_assert(Capacity && !(Capacity&(Capacity-1)));
    using Table=std::array<Handle,Capacity>;
    std::optional<Table> index_,wanted_;
    Handle first_{};
    std::size_t cursor_{},occupied_{};
    bool seen_{},saturated_{},filter_enabled_{},filter_attempted_{};
    ViewProofStats* stats_{};
    template<class Hash> static bool Contains(const Table& table,Handle h,Hash&& hash) noexcept {
        if(!h)return false;const auto slot=hash(h)&(Capacity-1);
        for(std::size_t i=0;i<Capacity;++i){const auto v=table[(slot+i)&(Capacity-1)];if(v==h)return true;if(!v)return false;}
        return false;
    }
    template<class Hash> static bool Put(Table& table,Handle h,Hash&& hash,bool* added=nullptr) noexcept {
        if(added)*added=false;if(!h)return true;const auto slot=hash(h)&(Capacity-1);
        for(std::size_t i=0;i<Capacity;++i){auto& v=table[(slot+i)&(Capacity-1)];if(v==h)return true;if(!v){v=h;if(added)*added=true;return true;}}
        return false;
    }
    template<class Hash> bool Insert(Handle h,Hash&& hash) noexcept {
        bool added=false;if(Put(*index_,h,hash,&added)){occupied_+=added;return true;}
        if(!saturated_ && stats_)++stats_->overflows;saturated_=true;return false;
    }
public:
    explicit PerDrawViewProof(ViewProofStats* stats=nullptr,bool filter=false) noexcept:filter_enabled_(filter),stats_(stats){}
    template<class Hash,class Full,class Walk,class BuildWanted>
    bool Query(Handle h,Hash&& hash,Full&& full,Walk&& walk,BuildWanted&& build_wanted) {
        if(stats_)++stats_->queries;
        if(first_ && first_==h){if(stats_)++stats_->first_hits;return true;}
        if(!seen_){seen_=true;if(stats_)++stats_->fallbacks;const bool ok=full(h);if(ok)first_=h;return ok;}
        if(wanted_ && !Contains(*wanted_,h,hash))wanted_.reset();
        if(!index_){index_.emplace();if(first_)Insert(first_,hash);}
        if(Contains(*index_,h,hash)){if(stats_)++stats_->index_hits;return true;}
        if(!saturated_){
            bool pause_for_filter=false;
            const auto visit=[&] {
                if(stats_)++stats_->walks;
                return walk(cursor_,h,[&](Handle value){
                    constexpr std::size_t threshold=Capacity/2 ? Capacity/2 : 1;
                    if(filter_enabled_ && !filter_attempted_ && occupied_>=threshold) {pause_for_filter=true;return false;}
                    if(wanted_ && !Contains(*wanted_,value,hash)){if(stats_)++stats_->filtered_views;return true;}
                    return Insert(value,hash);
                });
            };
            bool found=visit();
            if(pause_for_filter) {
                // The walk has returned and released views_mutex without
                // advancing its partially visited entry. Build only from the
                // draw-owned image infos, then restart that entry under lock.
                filter_attempted_=true;if(stats_)++stats_->wanted_builds;wanted_.emplace();
                if(!build_wanted([&](Handle v){return Put(*wanted_,v,hash);})) {
                    wanted_.reset();if(stats_)++stats_->wanted_overflows;
                } else if(!Contains(*wanted_,h,hash))wanted_.reset();
                if(wanted_) {
                    Table kept{};std::size_t count=0;
                    for(const auto v:*index_)if(v && Contains(*wanted_,v,hash)){bool added=false;Put(kept,v,hash,&added);count+=added;}
                    *index_=std::move(kept);occupied_=count;
                }
                pause_for_filter=false;found=visit();
            }
            if(found && !saturated_ && Contains(*index_,h,hash))return true;
        }
        if(stats_)++stats_->fallbacks;const bool ok=full(h);
        if(ok && !saturated_)Insert(h,hash);
        return ok;
    }
    template<class Hash,class Full,class Walk>
    bool Query(Handle h,Hash&& hash,Full&& full,Walk&& walk) {
        return Query(h,hash,full,walk,[](auto&&){return false;});
    }
};
}
