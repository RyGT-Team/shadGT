#pragma once
#include <utility>
namespace Native::Parallel {
// Only move the existing serial EndRendering across CPU-only proof preparation.
// No barrier is introduced; the caller retains the original trace precondition.
template<class State,class End> bool CloseChangedSerialScope(bool enabled,bool active,bool rendering,
                                                             const State& current,const State& next,End&& end) {
    if(!enabled || active || !rendering || current==next)return false;
    std::forward<End>(end)();return true;
}
}
