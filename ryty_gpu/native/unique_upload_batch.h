#pragma once
#include <algorithm>
#include <unordered_set>
namespace Native {
// Preserve first-admission order. A dirty write arriving during a snapshot must
// remain for the next image use, rather than re-enter through a duplicate slot.
template<class Sequence> void UniqueUploadBatch(Sequence& pending) {
    if (pending.size()<2) return;
    std::size_t used=0;
    if (pending.size()<=32) {
        for (const auto entry:pending) {
            if (std::find(pending.begin(),pending.begin()+used,entry)==pending.begin()+used)
                pending[used++]=entry;
        }
    } else {
        std::unordered_set<typename Sequence::value_type> seen;
        seen.reserve(pending.size());
        for (const auto entry:pending) if (seen.insert(entry).second) pending[used++]=entry;
    }
    pending.resize(used);
}
}
