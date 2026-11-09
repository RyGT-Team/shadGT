// Run in a disposable directory; tests the actual marker-driven recorder.
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "ryty_gpu/workload_trace.h"
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
    { ImageLife::Scope sentinel{"present"}; }
    std::snprintf(path,sizeof(path),"logs/image-lifecycle-%lu.csv",GetCurrentProcessId());
    for(unsigned i=0;i<600 && !(f=std::fopen(path,"r"));++i) Sleep(50);
    assert(f); std::fclose(f);
    bool route=false, fallback=false, probe=false, alias=false, unwanted=false, done=false;
    bool nodrops=false, take=false, before=false, after=false, retiring=false, create=false;
    bool allocation=false, heap=false, block=false; char line[1024];
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
            done |= std::strstr(line,",B,") && std::strstr(line,",present,");
        }
        std::fclose(f); if(!done) Sleep(50);
    }
    assert(done && route && fallback && probe && alias && !unwanted);
    assert(nodrops && take && before && after && retiring && create && allocation && heap && block);
}
