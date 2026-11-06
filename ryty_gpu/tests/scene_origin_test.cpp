#include "common/scene_origin.h"
#include <cassert>
#include <future>
#include <stdexcept>
int main() {
    using namespace SceneOrigin;
    const Origin outer{1,0,10,20,30,40},inner{2,3,50,60,70,0};
    assert(current.engine==0);
    try {Scope a{outer};assert(current.packet==10);{Scope b{inner};assert(current.vqid==3);}assert(current.packet==10);throw std::runtime_error("test");}catch(const std::runtime_error&){}
    assert(current.engine==0);
    Begin();auto f=std::async(std::launch::async,[=]{Scope s{inner};assert(current.packet==50);Event(current,"wait","decode",4,5,6);});
    f.get();assert(current.engine==0);
    for(int i=0;i<8193;++i)Event(outer,"wait","decode",4,5,6);
    {std::lock_guard lock{event_mutex};assert(event_count==events.size() && event_dropped==2);event_enabled=false;}
    if(preroll_enabled) {
        for(std::uint64_t i=0;i<5000;++i)Event(outer,"wait","preroll",4,i,6);
        Begin();
        assert(captured_preroll==preroll.size() && event_count==preroll.size());
        assert(captured_preroll_overwritten==5000-preroll.size());
        for(std::size_t i=0;i<preroll.size();++i)assert(events[i].value==5000-preroll.size()+i);
        Event(inner,"wait","live",4,5000,6);
        assert(event_count==preroll.size()+1 && events[preroll.size()].value==5000);
        // Beginning another capture must not replay the previous pre-roll.
        Begin();assert(captured_preroll==0 && event_count==0 && captured_preroll_overwritten==0);
    } else {
        Event(outer,"wait","disabled",4,5,6);
        Begin();assert(captured_preroll==0 && event_count==0 && captured_preroll_overwritten==0);
    }
}
