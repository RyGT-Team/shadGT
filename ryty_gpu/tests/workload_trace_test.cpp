#define RYTY_GPU
#include "../workload_trace.h"
#include <cassert>
static bool active = false;
namespace ImageLife { bool Enabled(const char*) { return active; } }
int main() {
    WorkloadTrace::Add(WorkloadTrace::Draws);
    auto zero=WorkloadTrace::Snapshot(); for(auto n:zero) assert(n==0);
    active=true;
    WorkloadTrace::Add(WorkloadTrace::Draws,3);
    WorkloadTrace::Add(WorkloadTrace::Dispatches,2);
    WorkloadTrace::Add(WorkloadTrace::ImageUploadRequestedBytes,4096);
    WorkloadTrace::Add(WorkloadTrace::HashBytes,8192);
    auto s=WorkloadTrace::Snapshot();
    assert(s[WorkloadTrace::Draws]==3 && s[WorkloadTrace::Dispatches]==2);
    assert(s[WorkloadTrace::ImageUploadRequestedBytes]==4096 && s[WorkloadTrace::HashBytes]==8192);
    auto same=WorkloadTrace::Snapshot(); assert(same==s);
}
