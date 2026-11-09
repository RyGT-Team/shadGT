// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/descriptor_set_memo.h"
#include <iostream>
#include <stdexcept>
int main() {
    Vulkan::DescriptorSetMemo<unsigned, 1> memo;
    unsigned cases=0;
    for (unsigned n=0;n<=512;++n) {
        std::vector<std::uint64_t> key(n,0xabcdef0123456789ull);
        memo.Store(7,key,19);
        if(memo.Find(7,key)!=19) throw std::runtime_error("equal key missed");
        if(memo.Find(8,key)!=0) throw std::runtime_error("different hash reused");
        for(unsigned i=0;i<n;++i) {
            key[i]^=1;
            if(memo.Find(7,key)!=0) throw std::runtime_error("same-hash changed word reused");
            key[i]^=1; ++cases;
        }
        key.push_back(0);
        if(memo.Find(7,key)!=0) throw std::runtime_error("different length reused");
        key.pop_back(); memo.Clear();
        if(memo.Find(7,key)!=0) throw std::runtime_error("retired set reused");
    }
    std::cout<<"PASS "<<cases<<" forced-collision word mutations plus hash, size, empty and retirement checks\n";
}
