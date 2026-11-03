// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <tuple>
#include <vector>
using u32 = std::uint32_t;
namespace Shader {
enum class SwStage { Fragment, TessellationControl, TessellationEval, Vertex, Geometry, Compute };
struct Info {
    bool attribute_flags_known{true}, resource_usage_known{true}, translation_failed{};
    SwStage sw_stage{SwStage::Vertex};
    bool has_fetch_shader{}, uses_patches{}, uses_dma{}, has_storage_images{}, uses_shader_clock{}, uses_direct{};
    std::vector<int> fmasks;
    struct Buffer { bool is_written{}; };
    std::vector<Buffer> buffers;
};
}
constexpr u32 MaxShaderStages = 6;
struct GraphicsPipeline {
#include "admission-stage-type.inc"
    std::array<const Shader::Info*, MaxShaderStages> stages{}, canonical_stages{};
    ParallelStageSummary parallel_stage_summary;
    auto GetStages() const { return std::span{stages.begin(), stages.end()-1}; }
    const auto& GetParallelStageSummary() const { return parallel_stage_summary; }
    int GetLayout() const { return 13; }
    void SetStageInfos(std::span<const Shader::Info*, MaxShaderStages> infos);
};
#include "admission-stage-build.inc"
static bool direct_enabled;
static bool ParallelD2Enabled() { return direct_enabled; }
static auto SelectStages(const GraphicsPipeline* pipeline, bool vertex_requested, bool has_vertex_input, bool vertex_safe) {
#include "admission-stage-select.inc"
    return std::pair{stages_safe, resource_key_known};
}
// Candidate100's complete predicate, including its independent resource-key result.
static auto OldStages(const GraphicsPipeline* pipeline, bool vertex_requested, bool has_vertex_input, bool vertex_safe) {
    bool stages_safe=true,resource_key_known=true;
    for(const auto* stage:pipeline->GetStages()) if(stage) {
        if(!stage->resource_usage_known) resource_key_known=false;
        if(!stage->fmasks.empty() || !stage->attribute_flags_known || !stage->resource_usage_known || stage->translation_failed ||
         (stage->sw_stage!=Shader::SwStage::Vertex && stage->sw_stage!=Shader::SwStage::Fragment) ||
         (stage->has_fetch_shader && !(vertex_requested && has_vertex_input && vertex_safe && stage->sw_stage==Shader::SwStage::Vertex)) || stage->uses_patches || stage->uses_dma ||
         (stage->uses_direct && !ParallelD2Enabled()) ||
         stage->has_storage_images || stage->uses_shader_clock ||
         std::ranges::any_of(stage->buffers,[](const auto& buffer){return buffer.is_written;})) stages_safe=false;
    }
    return std::pair{stages_safe, resource_key_known};
}
static void CheckStages(const GraphicsPipeline& pipeline) {
    for(unsigned live=0;live<16;++live) {
        direct_enabled=live&8;
        assert(SelectStages(&pipeline,live&1,live&2,live&4)==OldStages(&pipeline,live&1,live&2,live&4));
    }
}
struct Vertex {
    struct Row { unsigned binding{}; const void* pNext{}; };
    std::vector<int> buffers;
    std::vector<Row> input_bindings,attributes;
    std::vector<int> offsets,sizes,strides;
};
struct Bindings { std::vector<int> writes,buffers,images; };
struct Prepared;
struct Proof {
    const Prepared* prepared_identity{};
    const GraphicsPipeline* pipeline_identity{};
    int pipeline_layout{};
    const int* writes_data{}; size_t writes_size{};
    const int* buffers_data{}; size_t buffers_size{};
    const int* images_data{}; size_t images_size{};
    const Vertex* vertex_identity{};
    const int* vertex_buffers_data{}; size_t vertex_buffers_size{};
    bool vertex_shape{},vertex_bindings{},vertex_pnext{};
};
struct Prepared { bool has_vertices{true}; std::unique_ptr<Vertex> vertex_index=std::make_unique<Vertex>(); };
static void Produce(Prepared* p,Proof* candidate) {
#include "admission-vertex-build.inc"
}
static auto SelectVertex(Prepared* prepared, const GraphicsPipeline* pipeline, const Bindings& b, Proof* worker_result,
                         const Vertex* vi, bool vertex_requested=true,bool has_vertex_input=true,bool vertex_preflight=true) {
#include "admission-worker-identity.inc"
    bool vertex_shape_ok=false,vertex_safe=false,vertex_binding_ok=true,vertex_pnext_ok=true;
    struct { unsigned producer_vertex_hits{}; } counters;
    auto* parallel=&counters;
#include "admission-vertex-select.inc"
    }
    return std::tuple{vertex_shape_ok,vertex_binding_ok,vertex_pnext_ok,counters.producer_vertex_hits};
}
static bool OldShape(const Vertex& v) {
    return v.buffers.size()<=32 && v.input_bindings.size()<=32 && v.attributes.size()<=32 &&
        v.buffers.size()==v.offsets.size() && v.buffers.size()==v.sizes.size() && v.buffers.size()==v.strides.size() &&
        !v.buffers.empty() && v.buffers.size()==v.input_bindings.size() && v.buffers.size()==v.attributes.size();
}
static void Shape(Vertex& v,size_t n) {
    v.buffers.resize(n);v.input_bindings.resize(n);v.attributes.resize(n);v.offsets.resize(n);v.sizes.resize(n);v.strides.resize(n);
    for(size_t i=0;i<n;++i)v.input_bindings[i].binding=v.attributes[i].binding=static_cast<unsigned>(i);
}
int main() {
    GraphicsPipeline pipeline;
    std::array<const Shader::Info*,MaxShaderStages> stages{};
    pipeline.SetStageInfos(stages);CheckStages(pipeline);
    Shader::Info vertex,fragment;fragment.sw_stage=Shader::SwStage::Fragment;
    stages[0]=&fragment;stages[3]=&vertex;
    // All static hazards, both known flags, fetch, and direct requirements, plus combinations.
    for(unsigned bits=0;bits<(1u<<12);++bits) {
        vertex={};vertex.attribute_flags_known=!(bits&1);vertex.resource_usage_known=!(bits&2);
        vertex.translation_failed=bits&4;vertex.has_fetch_shader=bits&8;vertex.uses_patches=bits&16;
        vertex.uses_dma=bits&32;vertex.has_storage_images=bits&64;vertex.uses_shader_clock=bits&128;
        vertex.uses_direct=bits&256;if(bits&512)vertex.fmasks.push_back(1);
        vertex.buffers={{false},{bool(bits&1024)}};
        if(bits&2048)vertex.sw_stage=Shader::SwStage::Geometry;
        pipeline.SetStageInfos(stages);CheckStages(pipeline);
    }
    vertex={};fragment.has_fetch_shader=true;pipeline.SetStageInfos(stages);CheckStages(pipeline);
    fragment={};fragment.sw_stage=Shader::SwStage::Fragment;
    vertex.attribute_flags_known=false;pipeline.SetStageInfos(stages);assert(!pipeline.GetParallelStageSummary().known);
    vertex.attribute_flags_known=true;CheckStages(pipeline); // recovered metadata uses the unchanged live fallback
    vertex.resource_usage_known=false;pipeline.SetStageInfos(stages);vertex.resource_usage_known=true;CheckStages(pipeline);
    vertex.uses_direct=true;pipeline.SetStageInfos(stages);CheckStages(pipeline);assert(pipeline.GetParallelStageSummary().uses_direct);
    vertex.uses_direct=false;pipeline.SetStageInfos(stages);assert(!pipeline.GetParallelStageSummary().uses_direct);CheckStages(pipeline);
    Shader::Info compute;compute.translation_failed=true;stages[5]=&compute;pipeline.SetStageInfos(stages);CheckStages(pipeline);
    assert(pipeline.GetParallelStageSummary().safe); // graphics GetStages excludes compute

    Prepared p;Bindings b{{1},{2},{3}};
    const auto make_proof=[&] {
        Proof proof;proof.prepared_identity=&p;proof.pipeline_identity=&pipeline;proof.pipeline_layout=pipeline.GetLayout();
        proof.writes_data=b.writes.data();proof.writes_size=b.writes.size();proof.buffers_data=b.buffers.data();proof.buffers_size=b.buffers.size();
        proof.images_data=b.images.data();proof.images_size=b.images.size();Produce(&p,&proof);return proof;
    };
    for(size_t n:{0u,1u,32u,33u})for(unsigned mismatch=0;mismatch<7;++mismatch) {
        auto& v=*p.vertex_index;Shape(v,n);
        if(mismatch==1)v.offsets.push_back(0);if(mismatch==2)v.sizes.push_back(0);if(mismatch==3)v.strides.push_back(0);
        if(mismatch==4)v.input_bindings.push_back({});if(mismatch==5)v.attributes.push_back({});
        if(mismatch==6 && n)v.buffers.pop_back();
        auto proof=make_proof();
        for(unsigned identity=0;identity<13;++identity) {
            auto q=proof;
            if(identity==1)q.prepared_identity=nullptr;if(identity==2)q.pipeline_identity=nullptr;if(identity==3)++q.pipeline_layout;
            if(identity==4)q.writes_data=nullptr;if(identity==5)++q.writes_size;if(identity==6)q.buffers_data=nullptr;
            if(identity==7)++q.buffers_size;if(identity==8)q.images_data=nullptr;if(identity==9)++q.images_size;
            if(identity==10)q.vertex_identity=nullptr;if(identity==11)q.vertex_buffers_data=nullptr;if(identity==12)++q.vertex_buffers_size;
            assert(std::get<0>(SelectVertex(&p,&pipeline,b,&q,&v))==OldShape(v));
        }
        assert(std::get<0>(SelectVertex(&p,&pipeline,b,nullptr,&v))==OldShape(v));
        for(unsigned live=0;live<8;++live)
            assert(std::get<0>(SelectVertex(&p,&pipeline,b,&proof,&v,live&1,live&2,live&4))==(live==7 && OldShape(v)));
    }
    for(unsigned defect=0;defect<4;++defect) {
        auto& v=*p.vertex_index;Shape(v,1);
        if(defect==0)v.input_bindings[0].binding=1;if(defect==1)v.attributes[0].binding=1;
        if(defect==2)v.input_bindings[0].pNext=&p;if(defect==3)v.attributes[0].pNext=&p;
        auto proof=make_proof();auto fast=SelectVertex(&p,&pipeline,b,&proof,&v);
        auto fallback=SelectVertex(&p,&pipeline,b,nullptr,&v);
        assert(std::get<0>(fast) && std::get<0>(fallback));
        assert(std::get<1>(fast)==std::get<1>(fallback) && std::get<2>(fast)==std::get<2>(fallback));
        assert((defect<2 ? !std::get<1>(fast) : !std::get<2>(fast)));
    }
    Shape(*p.vertex_index,1);auto proof=make_proof();Vertex other;Shape(other,1);
    assert(!std::get<0>(SelectVertex(&p,&pipeline,b,&proof,&other)));
    p.has_vertices=false;assert(!std::get<0>(SelectVertex(&p,&pipeline,b,&proof,p.vertex_index.get())));
}
