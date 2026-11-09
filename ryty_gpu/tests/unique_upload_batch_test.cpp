// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <vector>
#include <random>
#include <cassert>
#include <cstdio>
#include "ryty_gpu/native/unique_upload_batch.h"
int main(){
    std::mt19937 random{0x6d39};
    for(unsigned trial=0;trial<100000;++trial){
        const unsigned length=trial<70?trial:random()%1025;
        std::vector<unsigned> values(length),expected;
        std::array<bool,2048> marked{};
        for(auto& value:values){value=random()%2048;if(!marked[value]){marked[value]=true;expected.push_back(value);}}
        Native::UniqueUploadBatch(values);assert(values==expected);
    }
    std::puts("PASS 100000 stable upload batch cases: inline/hash thresholds, empty/singleton, duplicates and first-admission order");
}
