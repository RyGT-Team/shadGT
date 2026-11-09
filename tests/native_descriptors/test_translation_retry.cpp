// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/translation_retry.h"
#include <stdexcept>
#include <iostream>
int main(){
    using namespace std::chrono;
    const steady_clock::time_point start{};auto deadline=start+milliseconds{250};
    unsigned cases=0;
    for(int ms=-1;ms<=1000;++ms)for(bool optional:{false,true})for(bool same:{false,true}) {
        bool delay=Vulkan::DelayFailedTranslationRetry(deadline,start+milliseconds{ms},optional,same);
        if(delay!=(optional&&same&&ms<250))throw std::runtime_error("retry bypass/deadline wrong");++cases;
    }
    // 10,000 identical draws in the cooldown must not become 10,000 failed compiles.
    unsigned attempts=1;
    for(unsigned i=0;i<10000;++i)if(!Vulkan::DelayFailedTranslationRetry(deadline,start+milliseconds{i/100},true,true))++attempts;
    if(attempts!=1)throw std::runtime_error("failure retry storm");
    std::cout<<"PASS "<<cases<<" deadline/required/changed-input cases and 10000-draw retry storm\n";
}
