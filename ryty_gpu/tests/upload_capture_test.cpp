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
    ImageLife::Initialize();
    char path[MAX_PATH]; std::snprintf(path,sizeof(path),"logs/image-lifecycle-%lu.start",GetCurrentProcessId());
    FILE* f=std::fopen(path,"w"); assert(f); std::fclose(f);
    for(unsigned i=0;i<100 && !ImageLife::Enabled("texture_source_result");++i) Sleep(50);
    assert(ImageLife::Enabled("texture_source_result"));
    assert(ImageLife::Enabled("workload_summary"));
    assert(!ImageLife::Enabled("origin_write_fault"));
    assert(ImageLife::SlowStart("producer_job_walk")==0);
    { ImageLife::Scope s{"texture_source_result",1,2,3,4,1}; }
    { ImageLife::Scope s{"texture_registry_fallback",1,2,3}; s.Finish(1,3); }
    { ImageLife::Scope omitted{"origin_write_fault"}; }
    { ImageLife::Scope sentinel{"present"}; }
    std::snprintf(path,sizeof(path),"logs/image-lifecycle-%lu.csv",GetCurrentProcessId());
    for(unsigned i=0;i<600 && !(f=std::fopen(path,"r"));++i) Sleep(50);
    assert(f); std::fclose(f);
    bool route=false, fallback=false, unwanted=false, done=false; char line[1024];
    for(unsigned i=0;i<100 && !done;++i) {
        f=std::fopen(path,"r"); assert(f);
        while(std::fgets(line,sizeof(line),f)) {
            route |= std::strstr(line,",B,") && std::strstr(line,",texture_source_result,1,2,3,4,1");
            fallback |= std::strstr(line,",B,") && std::strstr(line,",texture_registry_fallback,1,2,3,");
            unwanted |= std::strstr(line,"origin_write_fault") != nullptr;
            done |= std::strstr(line,",B,") && std::strstr(line,",present,");
        }
        std::fclose(f); if(!done) Sleep(50);
    }
    assert(done && route && fallback && !unwanted);
}
