// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/translation_priority.h"
#include <deque>
#include <numeric>
#include <stdexcept>
#include <iostream>
int main(){
    unsigned cases=0;
    for(int size=0;size<=128;++size)for(int target=-1;target<=size;++target){
        std::deque<int> queue(size);std::iota(queue.begin(),queue.end(),0);
        auto expected=queue;
        bool present=target>=0&&target<size;
        if(present){expected.erase(expected.begin()+target);expected.push_front(target);}
        if(Vulkan::PrioritizeTranslation(queue,target)!=present||queue!=expected)
            throw std::runtime_error("priority lost, duplicated or reordered speculative jobs");
        if(present){Vulkan::PrioritizeTranslation(queue,target);if(queue!=expected)throw std::runtime_error("repeat promotion changed queue");}
        ++cases;
    }
    std::cout<<"PASS "<<cases<<" translation priority, absent/running, empty and repeat cases\n";
}
