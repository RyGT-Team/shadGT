// Run in a disposable directory; tests the actual marker-driven recorder.
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <semaphore>
#include <thread>
#include <atomic>
#include "ryty_gpu/workload_trace.h"
#include "ryty_gpu/wait_trace.h"
#include "video_core/amdgpu/command_entry.h"
namespace Common { inline bool PerfFeatureEnabled(int) { return true; } }
struct TestDrawPipe {
    std::atomic<bool>* sender_returned{};
    void RunUrgentIfExclusive() {}
    void Push(Common::UniqueFunction<void> fn) { fn(); while(sender_returned && !sender_returned->load()) std::this_thread::yield(); }
};
struct Liverpool {
    std::mutex submit_mutex; std::condition_variable submit_cv;
    std::queue<AmdGpu::CommandEntry> command_queue; std::atomic<unsigned> num_commands{};
    TestDrawPipe* draw_pipe{}; unsigned commands_recorded{};
    void SyncRecorder(const char*) {}
    void ProcessCommands();
#include "send-command-under-test.inc"
};
#include "process-commands-under-test.inc"
int main() {
    SetEnvironmentVariableA("RYGT_IMAGE_LIFECYCLE_TRACE","1");
    SetEnvironmentVariableA("RYGT_IMAGE_TRACE_UPLOAD_ONLY","1");
    SetEnvironmentVariableA("RYGT_IMAGE_TRACE_WORKLOAD_ONLY","1");
    SetEnvironmentVariableA("RYGT_IMAGE_TRACE_CONTENT_PROBE","1");
    SetEnvironmentVariableA("RYGT_IMAGE_TRACE_ALIAS_PROBE","1");
    assert(!ImageLife::ContentProbeEnabled());
    assert(!ImageLife::AliasProbeEnabled());
    ImageLife::Initialize();
    assert(!ImageLife::ContentProbeEnabled());
    char path[MAX_PATH]; std::snprintf(path,sizeof(path),"logs/image-lifecycle-%lu.start",GetCurrentProcessId());
    FILE* f=std::fopen(path,"w"); assert(f); std::fclose(f);
    for(unsigned i=0;i<100 && !ImageLife::Enabled("texture_source_result");++i) Sleep(50);
    assert(ImageLife::Enabled("texture_source_result"));
    assert(ImageLife::Enabled("workload_summary"));
    assert(ImageLife::ContentProbeEnabled());
    assert(ImageLife::AliasProbeEnabled());
    assert(!ImageLife::Enabled("origin_write_fault"));
    assert(ImageLife::Enabled("queue_wait_summary"));
    assert(ImageLife::Enabled("direct_wait_summary"));
    assert(ImageLife::Enabled("unmap_phase_summary"));
    assert(ImageLife::Enabled("unmap_memory"));
    assert(ImageLife::SlowStart("producer_job_walk")==0);
    { ImageLife::Scope s{"texture_source_result",1,2,3,4,1}; }
    { ImageLife::Scope s{"texture_registry_fallback",1,2,3}; s.Finish(1,3); }
    { ImageLife::Scope s{"texture_content_probe",1,2,3,4,5}; s.Finish(6,3); }
    { ImageLife::Scope s{"texture_alias_probe",1,2,3,4,5}; s.Finish(5,1); }
    { ImageLife::Scope take{"upload_take", 10, 20};
      { ImageLife::Scope before{"upload_free_before_reclaim", take.Id(), 2, 30, 1, 16}; }
      { ImageLife::Scope after{"upload_free_after_reclaim", take.Id(), 3, 40, 2, 16}; }
      { ImageLife::Scope retiring{"upload_retiring", take.Id(), 4, 50, 60, 20}; }
      { ImageLife::Scope create{"upload_create_buffer", take.Id(), 10, 16};
        ImageLife::Scope allocation{"upload_allocation_info"};
        { ImageLife::Scope heap{"upload_heap_info", allocation.Id(), 2, 3, 4096, 1}; heap.Finish(7, 1); }
        { ImageLife::Scope block{"upload_block_info", allocation.Id(), 42, 1024, 5}; }
        allocation.Finish(16, 32);
        create.Finish(1, 16);
      }
      take.Finish(0, 16);
    }
    { ImageLife::Scope omitted{"origin_write_fault"}; }
    std::uint64_t command_parent=0;
    { Liverpool queue; bool invoked=false; std::atomic<bool> sender_returned{false};
      TestDrawPipe pipe{&sender_returned}; queue.draw_pipe=&pipe;
      std::thread worker([&] { std::unique_lock lk{queue.submit_mutex};
        queue.submit_cv.wait(lk,[&] { return !queue.command_queue.empty(); });
        lk.unlock(); queue.ProcessCommands(); });
      ImageLife::Scope unmap{"unmap_memory",123,456};
      command_parent=unmap.Id(); assert(command_parent);
      queue.SendCommand<true>([&] { invoked=true; },unmap.Id());
      sender_returned=true; assert(invoked); worker.join();
      invoked=false;
      queue.draw_pipe=nullptr;
      std::thread worker2([&] { std::unique_lock lk{queue.submit_mutex};
        queue.submit_cv.wait(lk,[&] { return !queue.command_queue.empty(); });
        lk.unlock(); queue.ProcessCommands(); });
      queue.SendCommand<true>([&] { invoked=true; },unmap.Id());
      assert(invoked); worker2.join();
      std::atomic<unsigned> async_count{};
      std::thread a([&] { for(unsigned i=0;i<64;++i) queue.SendCommand([&]{++async_count;}); });
      std::thread b([&] { for(unsigned i=0;i<64;++i) queue.SendCommand([&]{++async_count;}); });
      a.join(); b.join(); queue.ProcessCommands(); assert(async_count==128 && queue.num_commands==0);
    }
    { ImageLife::WaitAggregate a;
      for (unsigned i=0;i<256;++i) { const auto start=ImageLife::Timestamp(); a.Add("queue_wait_summary",42,1,start,start+1,false,3); }
      const auto start=ImageLife::Timestamp(); Sleep(2); a.Add("queue_wait_summary",42,1,start,ImageLife::Timestamp(),false,3);
    }
    { ImageLife::DirectWaitTrace t{44,2}; t.Acquired(); Sleep(2); t.BeforeUnlock(); }
    { ImageLife::Scope scope{"unmap_memory", 123, 456};
      ImageLife::WaitAggregate a;
      for (unsigned i=0;i<256;++i) { const auto start=ImageLife::Timestamp(); a.Add("unmap_phase_summary",77,4,start,start+1); }
      const auto start=ImageLife::Timestamp(); Sleep(2); a.Add("unmap_phase_summary",77,4,start,ImageLife::Timestamp()); }
    { ImageLife::WaitAggregate a;
      for (unsigned i=0;i<256;++i) { const auto start=ImageLife::Timestamp(); a.Add("unmap_phase_summary",78,6,start,start+1); } }
    { ImageLife::Scope sentinel{"present"}; }
    std::snprintf(path,sizeof(path),"logs/image-lifecycle-%lu.csv",GetCurrentProcessId());
    for(unsigned i=0;i<600 && !(f=std::fopen(path,"r"));++i) Sleep(50);
    assert(f); std::fclose(f);
    bool route=false, fallback=false, probe=false, alias=false, unwanted=false, done=false;
    bool nodrops=false, take=false, before=false, after=false, retiring=false, create=false;
    bool allocation=false, heap=false, block=false, queue_summary=false, queue_detail=false, direct_hold=false, window=false;
    bool unmap=false, unmap_summary=false, unmap_detail=false, export_summary=false;
    bool command_submit=false, command_callback=false, handoff_push=false, handoff_direct=false; char line[1024];
    for(unsigned i=0;i<100 && !done;++i) {
        f=std::fopen(path,"r"); assert(f);
        while(std::fgets(line,sizeof(line),f)) {
            route |= std::strstr(line,",B,") && std::strstr(line,",texture_source_result,1,2,3,4,1");
            fallback |= std::strstr(line,",B,") && std::strstr(line,",texture_registry_fallback,1,2,3,");
            probe |= std::strstr(line,",B,") && std::strstr(line,",texture_content_probe,1,2,3,4,5");
            alias |= std::strstr(line,",B,") && std::strstr(line,",texture_alias_probe,1,2,3,4,5");
            unwanted |= std::strstr(line,"origin_write_fault") != nullptr;
            nodrops |= std::strstr(line,"dropped=0,") != nullptr;
            take |= std::strstr(line,",B,") && std::strstr(line,",upload_take,10,20,0,0,0");
            before |= std::strstr(line,",B,") && std::strstr(line,",upload_free_before_reclaim,");
            after |= std::strstr(line,",B,") && std::strstr(line,",upload_free_after_reclaim,");
            retiring |= std::strstr(line,",B,") && std::strstr(line,",upload_retiring,");
            create |= std::strstr(line,",E,") && std::strstr(line,",upload_create_buffer,1,16,");
            allocation |= std::strstr(line,",E,") && std::strstr(line,",upload_allocation_info,16,32,");
            heap |= std::strstr(line,",E,") && std::strstr(line,",upload_heap_info,7,1,");
            block |= std::strstr(line,",B,") && std::strstr(line,",42,1024,5,0");
            queue_summary |= std::strstr(line,",B,") && std::strstr(line,",queue_wait_summary,42,1,256,256,1");
            queue_detail |= std::strstr(line,",B,") && std::strstr(line,",queue_wait_detail,42,1,");
            unmap |= std::strstr(line,",B,") && std::strstr(line,",unmap_memory,123,456,");
            unmap_summary |= std::strstr(line,",B,") && std::strstr(line,",unmap_phase_summary,77,4,256,256,1");
            unmap_detail |= std::strstr(line,",B,") && std::strstr(line,",unmap_phase_detail,77,4,");
            export_summary |= std::strstr(line,",B,") && std::strstr(line,",unmap_phase_summary,78,6,256,256,1");
            if (const char* p=std::strstr(line,",wait_summary_window,")) {
                unsigned long long parent=0, first=0, last=0;
                window |= std::sscanf(p,",wait_summary_window,%llu,%llu,%llu",&parent,&first,&last)==3 &&
                          parent && first && last>=first;
            }
            direct_hold |= std::strstr(line,",B,") && std::strstr(line,",direct_hold_detail,44,2,");
            if (const auto* p=std::strstr(line,",unmap_command_submit,")) {
                unsigned long long id=0,begin=0,acquired=0,enqueued=0,unlocked=0;
                command_submit |= std::strstr(line,",B,") &&
                    std::sscanf(p,",unmap_command_submit,%llu,%llu,%llu,%llu,%llu",&id,&begin,&acquired,&enqueued,&unlocked)==5 &&
                    id==command_parent && begin && begin<=acquired && acquired<=enqueued && enqueued<=unlocked;
            }
            if (const auto* p=std::strstr(line,",unmap_command_callback,")) {
                unsigned long long id=0,begin=0,end=0,returned=0,tid=0;
                command_callback |= std::strstr(line,",B,") &&
                    std::sscanf(p,",unmap_command_callback,%llu,%llu,%llu,%llu,%llu",&id,&begin,&end,&returned,&tid)==5 &&
                    id==command_parent && begin && begin<=end && end<=returned && tid && tid!=GetCurrentThreadId();
            }
            if (const auto* p=std::strstr(line,",unmap_command_handoff,")) {
                unsigned long long id=0,dequeued=0,begin=0,end=0,mode=0;
                const bool valid=std::strstr(line,",B,") &&
                    std::sscanf(p,",unmap_command_handoff,%llu,%llu,%llu,%llu,%llu",&id,&dequeued,&begin,&end,&mode)==5 &&
                    id==command_parent && dequeued && dequeued<=begin && begin<=end;
                handoff_push |= valid && mode==1; handoff_direct |= valid && mode==2;
            }
            done |= std::strstr(line,",B,") && std::strstr(line,",present,");
        }
        std::fclose(f); if(!done) Sleep(50);
    }
    assert(done && route && fallback && probe && alias && !unwanted);
    assert(nodrops && take && before && after && retiring && create && allocation && heap && block);
    assert(queue_summary && queue_detail && direct_hold && window);
    assert(unmap && unmap_summary && unmap_detail && export_summary);
    assert(command_submit && command_callback && handoff_push && handoff_direct);
}
