// Run in a disposable directory; tests the actual marker-driven recorder.
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "ryty_gpu/workload_trace.h"
int main() {
    SetEnvironmentVariableA("RYGT_IMAGE_LIFECYCLE_TRACE","1");
    SetEnvironmentVariableA("RYGT_IMAGE_TRACE_WORKLOAD_ONLY","1");
    ImageLife::Initialize();
    char path[MAX_PATH]; std::snprintf(path,sizeof(path),"logs/image-lifecycle-%lu.start",GetCurrentProcessId());
    FILE* f=std::fopen(path,"w"); assert(f); std::fclose(f);
    for(unsigned i=0;i<100 && !ImageLife::Enabled("workload_summary");++i) Sleep(50);
    assert(ImageLife::Enabled("workload_summary"));
    assert(!ImageLife::Enabled("producer_job_walk"));
    assert(ImageLife::SlowStart("producer_job_walk")==0);
    WorkloadTrace::Add(WorkloadTrace::Draws,7);
    auto counts=WorkloadTrace::Snapshot(); assert(counts[WorkloadTrace::Draws]==7);
    { ImageLife::Scope s{"workload_summary",1,2,counts[0],0,0}; s.Finish(0); }
    { ImageLife::Scope omitted{"origin_write_fault"}; }
    { ImageLife::Scope sentinel{"present"}; }
    std::snprintf(path,sizeof(path),"logs/image-lifecycle-%lu.csv",GetCurrentProcessId());
    for(unsigned i=0;i<600 && !(f=std::fopen(path,"r"));++i) Sleep(50);
    assert(f); std::fclose(f);
    bool summary=false, unwanted=false, done=false; char line[1024];
    for(unsigned i=0;i<100 && !done;++i) {
        f=std::fopen(path,"r"); assert(f);
        while(std::fgets(line,sizeof(line),f)) {
            summary |= std::strstr(line,",B,") && std::strstr(line,",workload_summary,1,2,7,0,0");
            unwanted |= std::strstr(line,"origin_write_fault") != nullptr;
            done |= std::strstr(line,",B,") && std::strstr(line,",present,");
        }
        std::fclose(f); if(!done) Sleep(50);
    }
    assert(done && summary && !unwanted);
}
