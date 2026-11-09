#include "video_core/renderer_vulkan/upload_chunk_cache.h"
#include <cassert>
#include <deque>
#include <memory>
#include <vector>
#include <iostream>
struct Buffer {
    std::uint64_t bytes;
    std::uint64_t Size() const { return bytes; }
};
struct Chunk { std::unique_ptr<Buffer> buffer; std::uint64_t retire_tick; };
int main() {
    using namespace Vulkan;
    constexpr auto MiB = 1ull<<20;
    assert(UploadChunkCapacity(0x1556800,8*MiB)==32*MiB);
    assert(UploadChunkCapacity(64*MiB+1,8*MiB)==64*MiB+1);
    std::vector<Chunk> free;
    std::deque<Chunk> retired;
    for (int i=0;i<16;++i) free.push_back({std::make_unique<Buffer>(8*MiB),0});
    auto large=std::make_unique<Buffer>(32*MiB); auto* identity=large.get();
    retired.push_back({std::move(large),10});
    CacheCompletedUploadChunks(free,retired,9);
    assert(retired.size()==1 && free.size()==16);
    CacheCompletedUploadChunks(free,retired,10);
    assert(retired.empty() && free.back().buffer.get()==identity);
    std::uint64_t rng=1234567;
    for (std::uint64_t tick=11; tick<100011; ++tick) {
        rng=rng*6364136223846793005ull+1;
        auto size=(1ull<<(rng%8))*MiB;
        retired.push_back({std::make_unique<Buffer>(size),tick});
        CacheCompletedUploadChunks(free,retired,tick-1);
        assert(!retired.empty() && retired.back().retire_tick==tick);
        std::uint64_t bytes=0;
        for (const auto& chunk:free) {
            assert(chunk.retire_tick<tick);
            assert(chunk.buffer->Size()<=UploadCacheMaxChunk);
            bytes+=chunk.buffer->Size();
        }
        assert(bytes<=UploadCacheBudget);
    }
    CacheCompletedUploadChunks(free,retired,100011);
    assert(retired.empty());
    std::cout<<"PASS: 100000 mixed-size completion/reclamation/budget cases; large snapshots retained in a full small-buffer reserve\n";
}
