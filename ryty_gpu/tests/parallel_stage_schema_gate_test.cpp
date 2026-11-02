// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <ranges>
#include <span>
#include <vector>
using u64=std::uint64_t;
namespace Shader {
enum class SwStage {Vertex,Fragment,Other};
struct Buffer {bool is_written{};};
struct Info {u64 identity{1};std::vector<Buffer> buffers;std::vector<int> fmasks;bool attribute_flags_known{true},resource_usage_known{true},translation_failed{},has_fetch_shader{},uses_patches{},uses_dma{},uses_direct{},has_storage_images{},uses_shader_clock{};SwStage sw_stage{SwStage::Vertex};};
}
struct Schema {u64 identity{};size_t count{};};
struct Proof {bool descriptor_structure{true};std::array<Schema,3> stages{};};
struct Prepared {std::vector<int> descriptor_actions;};struct Bindings {std::vector<int> writes;};
struct Pipeline {std::vector<const Shader::Info*> stages;auto GetStages()const{return std::span{stages};}};
static bool direct_enabled=true;bool ParallelD2Enabled(){return direct_enabled;}
struct Result {bool safe,resource,schema;bool operator==(const Result&)const=default;};
static Result Current(Pipeline* pipeline,Proof* worker_result,Prepared* prepared,const Bindings& b,bool worker_identity,bool vertex_requested,bool has_vertex_input,bool vertex_safe) {
#include "stage-schema-gate-under-test.inc"
    return {stages_safe,resource_key_known,known_stage_schema};
}
static Result Original(Pipeline* pipeline,Proof* proof,Prepared* prepared,const Bindings& b,bool worker_identity,bool vertex_requested,bool has_vertex_input,bool vertex_safe) {
    bool safe=true,resource=true;
    for(const auto* stage:pipeline->GetStages())if(stage) {
        if(!stage->resource_usage_known)resource=false;
        if(!stage->fmasks.empty() || !stage->attribute_flags_known || !stage->resource_usage_known || stage->translation_failed ||
           (stage->sw_stage!=Shader::SwStage::Vertex && stage->sw_stage!=Shader::SwStage::Fragment) ||
           (stage->has_fetch_shader && !(vertex_requested && has_vertex_input && vertex_safe && stage->sw_stage==Shader::SwStage::Vertex)) || stage->uses_patches || stage->uses_dma ||
           (stage->uses_direct && !ParallelD2Enabled()) || stage->has_storage_images || stage->uses_shader_clock ||
           std::ranges::any_of(stage->buffers,[](const auto& buffer){return buffer.is_written;}))safe=false;
    }
    bool schema=worker_identity && proof->descriptor_structure && prepared->descriptor_actions.size()==b.writes.size();
    const auto stages=pipeline->GetStages();
    if(schema)for(size_t i=0;i<proof->stages.size();++i) {
        const auto* stage=i<stages.size()?stages[i]:nullptr;const auto& expected=proof->stages[i];
        if((stage && !stage->identity) || (stage?stage->identity:0)!=expected.identity || (stage?stage->buffers.size():0)!=expected.count){schema=false;break;}
    }
    return {safe,resource,schema};
}
int main() {
    Shader::Info a,b;a.buffers.resize(2);b.identity=2;b.sw_stage=Shader::SwStage::Fragment;b.buffers.resize(1);
    Pipeline pipeline{{&a,&b,nullptr}};Proof proof;proof.stages={Schema{1,2},Schema{2,1},Schema{0,0}};Prepared prepared;Bindings bindings;
    auto compare=[&] {for(bool identity:{false,true})for(bool vr:{false,true})for(bool vi:{false,true})for(bool vs:{false,true})for(bool direct:{false,true}) {direct_enabled=direct;assert(Current(&pipeline,&proof,&prepared,bindings,identity,vr,vi,vs)==Original(&pipeline,&proof,&prepared,bindings,identity,vr,vi,vs));}};
    compare();
    for(int change=0;change<14;++change) {auto old=a;switch(change) {
      case 0:a.identity=0;break;case 1:a.identity=99;break;case 2:a.buffers.push_back({});break;case 3:a.buffers[0].is_written=true;break;
      case 4:a.fmasks.push_back(1);break;case 5:a.attribute_flags_known=false;break;case 6:a.resource_usage_known=false;break;case 7:a.translation_failed=true;break;
      case 8:a.sw_stage=Shader::SwStage::Other;break;case 9:a.has_fetch_shader=true;break;case 10:a.uses_patches=true;break;case 11:a.uses_dma=true;break;
      case 12:a.uses_direct=true;break;case 13:a.has_storage_images=true;a.uses_shader_clock=true;break;
    }compare();a=old;}
    pipeline.stages={nullptr,&b};compare();pipeline.stages={&b,&a,nullptr};compare();
    pipeline.stages={&a};proof.stages[2]={9,1};compare();proof.stages[2]={0,0};
    Shader::Info extra;extra.identity=4;extra.uses_dma=true;pipeline.stages={&a,&b,nullptr,&extra};compare();
    pipeline.stages={&a,&b,nullptr};
    assert(Current(&pipeline,nullptr,nullptr,bindings,false,true,true,true)==Original(&pipeline,nullptr,nullptr,bindings,false,true,true,true));
    pipeline.stages={&a,&b,nullptr};prepared.descriptor_actions.push_back(1);compare();
}
