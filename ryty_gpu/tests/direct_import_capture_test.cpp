// Run in a disposable working directory; exercises the actual marker-driven recorder.
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "ryty_gpu/image_lifecycle.h"
int main() {
    SetEnvironmentVariableA("RYGT_IMAGE_LIFECYCLE_TRACE", "1");
    SetEnvironmentVariableA("RYGT_IMAGE_TRACE_IMPORT_ONLY", "1");
    SetEnvironmentVariableA("RYGT_IMAGE_TRACE_WORKLOAD_ONLY", "1");
    SetEnvironmentVariableA("RYGT_IMAGE_TRACE_UPLOAD_ONLY", "1");
    SetEnvironmentVariableA("RYGT_IMAGE_TRACE_CONTENT_PROBE", "1");
    ImageLife::Initialize();
    char path[MAX_PATH]; std::snprintf(path,sizeof(path),"logs/image-lifecycle-%lu.start",GetCurrentProcessId());
    FILE* f=std::fopen(path,"w"); assert(f); std::fclose(f);
    for(unsigned i=0;i<100 && !ImageLife::Enabled("direct_import_operation");++i) Sleep(50);
    assert(ImageLife::Enabled("direct_import_operation"));
    assert(!ImageLife::Enabled("producer_job_walk"));
    assert(!ImageLife::Enabled("workload_summary"));
    assert(!ImageLife::Enabled("texture_source_result"));
    assert(!ImageLife::ContentProbeEnabled());
    assert(ImageLife::SlowStart("producer_job_walk")==0);
    assert(ImageLife::Timestamp()!=0);
    { ImageLife::Scope s{"direct_import_operation",1,0,0,12,34}; s.Finish(1); }
    { ImageLife::Scope s{"producer_job_walk",0,0}; }
    { ImageLife::Scope s{"direct_import_sentinel"}; }
    std::snprintf(path,sizeof(path),"logs/image-lifecycle-%lu.csv",GetCurrentProcessId());
    for(unsigned i=0;i<600 && !(f=std::fopen(path,"r"));++i) Sleep(50);
    assert(f); std::fclose(f);
    bool op=false, other=false, done=false; char line[1024];
    for(unsigned i=0;i<100 && !done;++i) {
        f=std::fopen(path,"r"); assert(f);
        while(std::fgets(line,sizeof(line),f)) {
            op |= std::strstr(line,",B,") && std::strstr(line,",direct_import_operation,1,0,0,12,34");
            other |= std::strstr(line,"producer_job_walk") != nullptr;
            done |= std::strstr(line,",B,") && std::strstr(line,",direct_import_sentinel,");
        }
        std::fclose(f); if(!done) Sleep(50);
    }
    assert(done && op && !other);
}
