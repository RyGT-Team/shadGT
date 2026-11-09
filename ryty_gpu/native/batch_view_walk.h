#pragma once
#include <cstddef>
namespace Native::Parallel {
// Caller holds the view mutex. Incomplete entries are retried, and no registry pointer is retained.
template<class Handle,class Entry,class Add>
bool BatchViewWalk(std::size_t& cursor,std::size_t count,Handle wanted,Entry&& entry,Add&& add) {
    std::size_t entries=0,records=0; bool found=false,add_failed=false;
    while(cursor<count && entries<16) {
        const bool complete=entry(cursor,[&](bool sampled,Handle h) {
            if(records==64)return false;
            ++records;
            if(sampled) { if(!add(h)){add_failed=true;return false;} found|=h==wanted; }
            return true;
        });
        if(!complete)return !add_failed && found;
        ++cursor;++entries;
    }
    return found;
}
}
