// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/immutable_upload_memo.h"
#include <array>
#include <cassert>
#include <cstdint>
int main() {
    Vulkan::ImmutableUploadMemo<unsigned,2> memo;
    std::array<std::uint8_t,4> a{1,2,3,4},b{4,3,2,1};
    const auto hash=[](const void*,size_t){return std::uint64_t{7};};
    auto unproven=memo.Find(0,a,hash);assert(!unproven.binding);
    memo.Store(unproven,0,a,11,false);auto bad=memo.Find(0,a,hash);assert(bad.binding && *bad.binding==11 && !bad.backing_proven);
    auto proven=memo.Find(0,b,hash);memo.Store(proven,0,b,12,true);auto good=memo.Find(0,b,hash);assert(good.binding && *good.binding==12 && good.backing_proven);
    auto old=memo.Find(0,a,hash);assert(old.binding && !old.backing_proven);
    memo.Clear();assert(!memo.Find(0,b,hash).binding);
}
