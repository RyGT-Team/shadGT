// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/translation_retry.h"
#include <algorithm>
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

    // FIX-SGT-094: walker storage refusals.
    using Vulkan::WalkerGate;
    for(bool refused:{false,true})for(bool fit:{false,true})for(bool wall:{false,true}){
        Vulkan::WalkerWallBudget budget{};
        const auto gate=Vulkan::DecideWalkerGate(refused,fit,wall,budget,start+seconds{1});
        const auto expected=refused&&!fit?WalkerGate::SkipRefused:WalkerGate::Translate;
        if(gate!=expected)throw std::runtime_error("walker gate decision wrong");
    }
    // A refused shader is not translated again on any of 10,000 draws while nothing could fit.
    Vulkan::WalkerWallBudget wall{};
    unsigned translated=0;
    for(unsigned i=0;i<10000;++i)
        if(Vulkan::DecideWalkerGate(true,false,true,wall,start+milliseconds{i})==WalkerGate::Translate)++translated;
    if(translated!=0)throw std::runtime_error("refused walker retried");
    // At the wall, new shaders whose translations are refused (2 ms each) get at most the budget
    // per window, plus the one attempt that crosses it, on every draw for 10 s.
    wall={};
    auto now=start+seconds{1};
    steady_clock::duration window_spent{},worst{};
    auto window_start=now;
    unsigned windows=0;
    for(unsigned i=0;i<10000;++i,now+=milliseconds{1}){
        if(now-window_start>=Vulkan::WalkerWallBudget::window){
            worst=std::max(worst,window_spent);window_spent={};window_start=now;++windows;
        }
        if(Vulkan::DecideWalkerGate(false,false,true,wall,now)!=WalkerGate::Translate)continue;
        Vulkan::ChargeWalkerWall(wall,now,now+milliseconds{2});
        window_spent+=milliseconds{2};
    }
    worst=std::max(worst,window_spent);
    if(worst>Vulkan::WalkerWallBudget::budget+milliseconds{2}||worst<Vulkan::WalkerWallBudget::budget)
        throw std::runtime_error("walker wall budget wrong");
    // Away from the wall a translation is never held back by the budget.
    for(unsigned i=0;i<100;++i)
        if(Vulkan::DecideWalkerGate(false,false,false,wall,now)!=WalkerGate::Translate)throw std::runtime_error("budget outside the wall");
    std::cout<<"PASS walker gate: refusals, 10000 refused draws, wall budget over "<<windows<<" windows (worst "
             <<duration_cast<milliseconds>(worst).count()<<" ms)\n";
}
