#include "video_core/renderer_vulkan/upload_chunk_tail.h"
#include <vector>
#include <deque>
#include <memory>
#include <random>
#include <cassert>
#include <cstdio>
struct Buffer { unsigned long long size,id; auto Size() const { return size; } };
struct Chunk { std::unique_ptr<Buffer> buffer; unsigned long long retire_tick=0; };
int main() {
 constexpr std::uint64_t size=8<<20;
 std::vector<Chunk> chunks; std::deque<Chunk> retired; std::uint64_t used=0,id=0;
 auto make=[&](std::uint64_t n){return Chunk{std::make_unique<Buffer>(Buffer{n,++id}),0};};
 auto splice=[&](std::uint64_t tick){Vulkan::RetireUploadChunks(chunks,retired,used,tick,size);};
 splice(1);assert(chunks.empty()&&retired.empty()&&used==0);
 chunks.push_back(make(size));used=48;auto first=chunks[0].buffer->id;
 for(int i=1;i<=10000;++i){splice(i);assert(chunks.size()==1&&chunks[0].buffer->id==first&&used==48&&retired.empty());}
 used=size;splice(10001);assert(chunks.empty()&&used==0&&retired.back().retire_tick==10001&&retired.back().buffer->id==first);
 chunks.push_back(make(size+1));used=1;splice(10002);assert(chunks.empty()&&used==0&&retired.back().retire_tick==10002);
 std::mt19937_64 rng(123);std::uint64_t tick=10003,allocations=0,last_end=0,current=0;
 for(int i=0;i<100000;++i){
   auto length=1+rng()%(1<<18);auto align=std::uint64_t(16)<<(rng()%9);
   auto offset=(used+align-1)&~(align-1);
   if(chunks.empty()||offset+length>chunks.back().buffer->Size()) {chunks.push_back(make(size));++allocations;used=offset=0;}
   auto handle=chunks.back().buffer->id;
   if(handle==current)assert(offset>=last_end);
   used=offset+length;assert(used<=size);current=handle;last_end=used;
   splice(tick++);assert(chunks.size()==1&&chunks[0].buffer->id==handle&&used==last_end);
   // Chunks used by earlier lists must remain alive until their final use tick.
   for(auto& c:retired)assert(c.buffer&&c.retire_tick<tick);
   if(retired.size()>4)retired.pop_front();
 }
 assert(allocations<2000);used=size;splice(tick);assert(chunks.empty()&&retired.back().retire_tick==tick);
 std::printf("PASS: 100000 aligned allocations, 10000 empty-list tails, full/oversize retirement, latest-use ticks; %llu backing chunks vs 100000 per-list chunks\n",allocations);
}
