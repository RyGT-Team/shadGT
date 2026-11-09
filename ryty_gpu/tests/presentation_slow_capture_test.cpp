// Windows integration test of the real marker-driven recorder; run from a disposable directory.
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "ryty_gpu/image_lifecycle.h"
int main() {
    SetEnvironmentVariableA("RYGT_IMAGE_LIFECYCLE_TRACE", "1");
    SetEnvironmentVariableA("RYGT_IMAGE_TRACE_PRESENT_ONLY", "1");
    ImageLife::Initialize();
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "logs/image-lifecycle-%lu.start", GetCurrentProcessId());
    FILE* marker = std::fopen(path, "w"); assert(marker); std::fclose(marker);
    for (unsigned i=0; i<100 && !ImageLife::Enabled("producer_job_walk"); ++i) Sleep(50);
    assert(ImageLife::Enabled("producer_job_walk"));
    assert(ImageLife::SlowStart("producer_job_walk") != 0);
    assert(ImageLife::SlowStart("origin_write_fault") == 0);
    assert(ImageLife::SlowStart(nullptr) == 0);
    { ImageLife::SlowScope span{"producer_job_walk", 0, 0}; Sleep(2); }
    { ImageLife::Scope omitted{"origin_write_fault", 0, 0}; }
    { ImageLife::Scope sentinel{"present", 0, 0}; }
    std::snprintf(path, sizeof(path), "logs/image-lifecycle-%lu.csv", GetCurrentProcessId());
    FILE* output = nullptr;
    for (unsigned i=0; i<600 && !(output=std::fopen(path,"r")); ++i) Sleep(50);
    assert(output);
    std::fclose(output);
    bool slow=false, unwanted=false, done=false; char line[1024];
    for (unsigned i=0; i<100 && !done; ++i) {
        output=std::fopen(path,"r"); assert(output);
        while (std::fgets(line,sizeof(line),output)) {
            slow |= std::strstr(line,",S,0,producer_job_walk,") != nullptr;
            unwanted |= std::strstr(line,"origin_write_fault") != nullptr;
            done |= std::strstr(line,",B,") && std::strstr(line,",present,");
        }
        std::fclose(output);
        if (!done) Sleep(50);
    }
    assert(done && slow && !unwanted);
}
