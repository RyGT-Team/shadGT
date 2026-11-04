#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <cstddef>

struct Pipeline {
    mutable unsigned layout_reads{};
    unsigned GetLayout() const { ++layout_reads; return 9; }
    unsigned PushSet() const { return 3; }
};
struct PreparedDraw {
    struct CpuProof {
        const void* prepared_identity{}; const void* pipeline_identity{};
        unsigned pipeline_layout{};
        const void* writes_data{}; std::size_t writes_size{};
        const void* buffers_data{}; std::size_t buffers_size{};
        const void* images_data{}; std::size_t images_size{};
        bool descriptor_structure{},counts_valid{}; unsigned counts_push{};
        std::array<const void*,4> group_data{};
        std::array<std::size_t,4> group_sizes{};
        std::size_t ordinary_writes{},ordinary_buffers{},ordinary_images{};
    } cpu_proof;
    const Pipeline* pipeline{};
};
struct Bindings {
    std::vector<int> writes{1},buffers{2,3},images{4,5,6};
    std::array<std::vector<int>,4> grouped_writes;
    bool groups_ready=true;
};
struct BindingCapture {
    bool counts_proven{};
    std::size_t ordinary_writes{},ordinary_buffers{},ordinary_images{};
};
struct Case {
    Pipeline pipeline;
    PreparedDraw owned;
    PreparedDraw* prepared=&owned;
    void* parallel=this;
    bool from_prepared=true;
    Bindings bindings;
    Case() {
        owned.pipeline=&pipeline;
        auto& p=owned.cpu_proof; auto& b=bindings;
        p.prepared_identity=&owned;p.pipeline_identity=&pipeline;p.pipeline_layout=9;
        p.writes_data=b.writes.data();p.writes_size=b.writes.size();
        p.buffers_data=b.buffers.data();p.buffers_size=b.buffers.size();
        p.images_data=b.images.data();p.images_size=b.images.size();
        p.descriptor_structure=p.counts_valid=true;p.counts_push=3;
        p.ordinary_writes=4;p.ordinary_buffers=5;p.ordinary_images=6;
        for(std::size_t i=0;i<4;++i) {
            b.grouped_writes[i].push_back(int(i));
            p.group_data[i]=b.grouped_writes[i].data();p.group_sizes[i]=1;
        }
    }
    Case(const Case&)=delete;
};
struct Result {
    bool identity{},counts{};
    std::size_t writes{},buffers{},images{};
    unsigned layout_reads{};
    bool operator==(const Result&) const = default;
};
#include "worker-identity-defer.inc"
static void Check(bool ok,const char* why) {
    if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(9);}
}
int main(int argc,char** argv) {
    const bool old_negative=argc>1 && std::string_view(argv[1])=="--eager-negative";
    {
        Case c;
        auto r=old_negative?Eager(c,true):Current(c,true);
        Check(r.layout_reads==0 && !r.identity && !r.counts,"cheap rejection must not touch worker proof/pipeline layout");
    }
    for(unsigned mutation=0;mutation<23;++mutation) {
        Case c; auto& p=c.owned.cpu_proof;
        switch(mutation) {
        case 1:p.prepared_identity=nullptr;break;
        case 2:p.pipeline_identity=nullptr;break;
        case 3:p.pipeline_layout=10;break;
        case 4:p.writes_data=nullptr;break;
        case 5:++p.writes_size;break;
        case 6:p.buffers_data=nullptr;break;
        case 7:++p.buffers_size;break;
        case 8:p.images_data=nullptr;break;
        case 9:++p.images_size;break;
        case 10:p.descriptor_structure=false;break;
        case 11:p.counts_valid=false;break;
        case 12:c.bindings.groups_ready=false;break;
        case 13:p.counts_push=2;break;
        case 14:p.group_data[0]=nullptr;break;
        case 15:++p.group_sizes[3];break;
        case 16:c.prepared=nullptr;break;
        case 17:c.parallel=nullptr;break;
        case 18:c.from_prepared=false;break;
        case 19:c.owned.pipeline=nullptr;break;
        case 20:p.group_data[3]=nullptr;break;
        case 21:++p.group_sizes[0];break;
        case 22:++p.ordinary_images;break;
        }
        const auto a=Eager(c,false);c.pipeline.layout_reads=0;
        const auto b=Current(c,false);
        Check(a==b,"accepted path must retain exact identity/count predicate and values");
        if(mutation==0)Check(b.identity && b.counts && b.writes==4 && b.buffers==5 && b.images==6,"valid exact snapshot retained");
        if((mutation>=1 && mutation<=9)||(mutation>=16 && mutation<=19))Check(!b.identity && !b.counts,"identity mutation rejected");
        if(mutation>=10 && mutation<=15)Check(b.identity && !b.counts,"count/shape mutation retains fallback");
        if(mutation==20||mutation==21)Check(b.identity && !b.counts,"every group remains checked");
    }
    // Rejection remains safe even when no prepared/parallel state exists.
    Case absent;absent.prepared=nullptr;absent.parallel=nullptr;
    Check(Current(absent,true).layout_reads==0,"rejected absent state is untouched");
    std::puts("PASS: real deferred identity/count proof, eager negative, exact accepted paths and mutated snapshots");
}
