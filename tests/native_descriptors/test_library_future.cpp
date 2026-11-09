// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/shared_creation_cache.h"
#include <iostream>
#include <stdexcept>
#include <vector>
int main(){
    Vulkan::SharedCreationCache<int,int> cache;
    if(cache.Pending(1).valid())throw std::runtime_error("absent key pending");
    std::promise<void> entered,release;auto released=release.get_future().share();
    auto owner=std::async(std::launch::async,[&]{return cache.GetOrCreate(1,[&]{entered.set_value();released.wait();return std::make_shared<int>(42);});});
    entered.get_future().wait();
    std::vector<std::shared_future<std::shared_ptr<int>>> waiters;
    for(int i=0;i<128;++i){auto f=cache.Pending(1);if(!f.valid())throw std::runtime_error("in-flight future absent");waiters.push_back(f);}
    auto independent=cache.GetOrCreate(2,[]{return std::make_shared<int>(19);});
    release.set_value();auto value=owner.get();
    for(auto& f:waiters)if(f.get()!=value)throw std::runtime_error("duplicate result");
    if(cache.Pending(1).valid()||cache.Find(1)!=value||*independent!=19)throw std::runtime_error("completion state wrong");
    waiters.clear();value.reset();if(cache.Find(1))throw std::runtime_error("resource lifetime extended");
    std::promise<void> fail_entered,fail_release;auto gate=fail_release.get_future().share();
    auto failed=std::async(std::launch::async,[&]{try{cache.GetOrCreate(3,[&]()->std::shared_ptr<int>{fail_entered.set_value();gate.wait();throw std::runtime_error("expected");});}catch(const std::runtime_error&){};});
    fail_entered.get_future().wait();auto pending=cache.Pending(3);fail_release.set_value();failed.get();
    bool caught=false;try{pending.get();}catch(const std::runtime_error&){caught=true;}
    if(!caught||cache.Pending(3).valid()||*cache.GetOrCreate(3,[]{return std::make_shared<int>(7);})!=7)throw std::runtime_error("exception/retry failed");
    std::cout<<"PASS 128 shared pending consumers, independent creation, weak lifetime, failure and retry\n";
}
