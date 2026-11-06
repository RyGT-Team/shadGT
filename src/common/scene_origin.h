#pragma once
#ifndef NDEBUG
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <array>
#include <mutex>
#include <filesystem>
#include <cstdio>
#include <chrono>
#include <vector>
namespace SceneOrigin {
struct Origin {std::uint32_t engine{},vqid{};std::uint64_t packet{},buffer{},sequence{},submission{};};
inline thread_local Origin current{};
inline std::atomic<std::uint64_t> next_buffer_sequence{1};
struct EventRow {Origin origin;const char* kind;const char* outcome;std::uint64_t address,value,mask,host_ns;};
inline std::mutex event_mutex;
inline std::array<EventRow,8192> events{};
inline std::size_t event_count{},event_dropped{};
inline std::atomic<bool> event_enabled{};
inline void Begin() {std::lock_guard lock{event_mutex};event_count=event_dropped=0;event_enabled.store(true,std::memory_order_release);}
// kind and outcome must be static string literals; events retain these pointers.
inline void Event(Origin origin,const char* kind,const char* outcome,std::uint64_t address,std::uint64_t value,std::uint64_t mask) {
    if(!event_enabled.load(std::memory_order_acquire))return;
    std::lock_guard lock{event_mutex};if(!event_enabled.load(std::memory_order_relaxed))return;
    if(event_count==events.size()){++event_dropped;return;}
    const auto now=std::chrono::steady_clock::now().time_since_epoch();
    events[event_count++]={origin,kind,outcome,address,value,mask,std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count())};
}
inline void Save(const std::filesystem::path& dir) {
    std::vector<EventRow> snapshot;std::size_t count=0,dropped=0;
    {std::lock_guard lock{event_mutex};event_enabled.store(false,std::memory_order_release);count=event_count;dropped=event_dropped;snapshot.assign(events.begin(),events.begin()+count);}
    if(auto* f=std::fopen((dir/"async-dependencies.csv").string().c_str(),"w")) {
        std::fprintf(f,"host_ns,engine,vqid,packet,buffer,buffer_sequence,graphics_submission,kind,outcome,address,value,mask\n");
        for(std::size_t i=0;i<count;++i){const auto& e=snapshot[i];std::fprintf(f,"%llu,%u,%u,0x%llx,0x%llx,%llu,%llu,%s,%s,0x%llx,0x%llx,0x%llx\n",e.host_ns,e.origin.engine,e.origin.vqid,e.origin.packet,e.origin.buffer,e.origin.sequence,e.origin.submission,e.kind,e.outcome,e.address,e.value,e.mask);}std::fclose(f);
    }
    if(auto* f=std::fopen((dir/"async-dependencies-status.txt").string().c_str(),"w")) {std::fprintf(f,"events=%zu dropped=%zu coverage=partial_no_preroll buffer_sequence=decode_invocation_not_submission asc_submission=unknown engine=0_unknown_1_graphics_2_asc host_gpu_queue=0\nwrite_data value=first_dword mask=byte_count; release_mem and event_eop mask=data_selector; semaphore value_and_mask=unknown; generic event_write, graphics wait completion and some other completion paths are not captured. Missing events do not establish independence. Host steady-clock and GPU timestamps are not calibrated to each other.\n",count,dropped);std::fclose(f);}
}
class Scope {
    Origin previous;
public:
    explicit Scope(Origin value) noexcept:previous(current){current=value;}
    ~Scope(){current=previous;}
    Scope(const Scope&)=delete;
};
}
#define SCENE_SEQUENCE_DECLARE const auto scene_sequence=SceneOrigin::next_buffer_sequence.fetch_add(1,std::memory_order_relaxed)
#define SCENE_ORIGIN_DECLARE(name, engine_, vqid_, packet_, buffer_) const SceneOrigin::Origin name{engine_,vqid_,std::uint64_t(packet_),std::uint64_t(buffer_),scene_sequence,(engine_==1?decoding_submit.load(std::memory_order_relaxed):0)}
#define SCENE_ORIGIN_CAPTURE(name) , name
#define SCENE_ORIGIN_SCOPE(name) SceneOrigin::Scope scene_origin_scope{name}
#define SCENE_EVENT(origin,kind,outcome,address,value,mask) SceneOrigin::Event(origin,kind,outcome,address,value,mask)
#else
#define SCENE_SEQUENCE_DECLARE
#define SCENE_ORIGIN_DECLARE(name, engine_, vqid_, packet_, buffer_)
#define SCENE_ORIGIN_CAPTURE(name)
#define SCENE_ORIGIN_SCOPE(name)
#define SCENE_EVENT(origin,kind,outcome,address,value,mask)
#endif
