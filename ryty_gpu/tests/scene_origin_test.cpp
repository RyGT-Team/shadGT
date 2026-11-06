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
}
