#pragma once
#include <cstddef>
namespace Native {
constexpr bool DescriptorCountsFit(std::size_t w,std::size_t b,std::size_t i,
                                   std::size_t remaining_w,std::size_t remaining_b,std::size_t remaining_i) noexcept {
    return w<=remaining_w && b<=remaining_b && i<=remaining_i;
}
// The write records are copied, and their binding-owned info arrays remain alive
// until this draw returns. No destination is ever left in the original records.
template<class Writes,class Set,class Updates>
void AppendDescriptorUpdates(const Writes& writes,Set set,Updates& updates) {
    for (const auto& original:writes) {
        auto copy=original;copy.dstSet=set;updates.push_back(copy);
    }
}
// All new sets are updated before recording any bind. Updating a set after it
// was bound would invalidate a command buffer without UPDATE_AFTER_BIND.
template<class Writes,class Sets,class Groups,class Update,class Push,class Bind>
void CommitDescriptorBatch(const Writes& updates,const Sets& sets,std::size_t push_set,
                           const Groups& groups,Update&& update,Push&& push,Bind&& bind) {
    if (!updates.empty()) update(updates);
    for (std::size_t i=0;i<groups.size();++i) {
        if (groups[i].empty()) continue;
        if (i==push_set) push(i,groups[i]);
        else if (sets[i]) bind(i,sets[i]);
    }
}
}
