// SPDX-License-Identifier: GPL-2.0-or-later
// Producer and query fragments are checked against renderer source.
#include "ryty_gpu/native/descriptor_snapshot.h"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>
#include <array>
#include <ranges>
#include <cstdio>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
static bool fail_alloc=false;void* operator new(size_t n){if(fail_alloc){fail_alloc=false;throw std::bad_alloc();}if(auto*p=malloc(n))return p;throw std::bad_alloc();}void operator delete(void*p) noexcept{free(p);}void operator delete(void*p,size_t) noexcept{free(p);}
struct Upload{u32 info{},size{};};struct Bindings{std::vector<vk::WriteDescriptorSet>writes;std::vector<vk::DescriptorBufferInfo>buffers;std::vector<vk::DescriptorImageInfo>images;};
namespace Shader {enum class BufferType { Other, DirectTable }; struct Buffer { BufferType buffer_type{}; bool is_written{}; bool IsSpecial()const{return false;} struct Sharp {u64 base_address{};u64 GetSize()const{return 0;}}; Sharp GetSharp(const auto&)const{return {};} }; struct Info { u64 identity{}; std::vector<Buffer> buffers; };}
struct PreparedDraw{struct DescriptorAction{std::size_t buffer_first{},stage_local{};u8 range_hint{255},region_hint{255};u32 stage_index{~u32{0}},upload_size{};u8 upload_match{2};bool is_buffer{};};struct Proof{struct StageSchema{u64 identity{};size_t count{},table_local{SIZE_MAX},table_upload{SIZE_MAX};};std::array<StageSchema,5> stages{};bool descriptor_structure{};}cpu_proof;Bindings bindings;std::vector<DescriptorAction>descriptor_actions;std::vector<Upload>uploads;std::vector<std::pair<u64,u64>>buffer_ranges;struct Region{u64 address,size;};std::vector<Region>regions;};
void produce(PreparedDraw* p){try{std::array<const Shader::Info*,5> stages{};auto*candidate=&p->cpu_proof;const auto& b=p->bindings;const auto buffers=std::span<const vk::DescriptorBufferInfo>{b.buffers.data(),b.buffers.size()};const auto images=std::span<const vk::DescriptorImageInfo>{b.images.data(),b.images.size()};candidate->descriptor_structure=true;
        size_t stage_base=0;
        for(size_t si=0;si<candidate->stages.size();++si) {
            auto& schema=candidate->stages[si];schema={};schema.table_upload=SIZE_MAX;
            if(si>=stages.size() || !stages[si])continue;
            const auto& stage=*stages[si];schema.identity=stage.identity;schema.count=stage.buffers.size();
            size_t table=SIZE_MAX;
            for(size_t j=0;j<schema.count;++j)if(stage.buffers[j].buffer_type==Shader::BufferType::DirectTable) {
                if(table!=SIZE_MAX){table=SIZE_MAX-1;break;}table=j;
            }
            if(table<schema.count && stage_base<=SIZE_MAX-table) {
                schema.table_local=table;
                const auto global=stage_base+table;
                for(size_t j=0;j<p->uploads.size();++j)if(p->uploads[j].info==global) {
                    if(schema.table_upload!=SIZE_MAX){schema.table_upload=SIZE_MAX-1;break;}
                    schema.table_upload=j;
                }
            }
            if(schema.count>SIZE_MAX-stage_base){candidate->descriptor_structure=false;break;}
            stage_base+=schema.count;
        }
        p->descriptor_actions.clear();
        const bool build_actions=candidate->descriptor_structure && b.writes.size()<=128;
        if(build_actions)p->descriptor_actions.reserve(b.writes.size());
        for(const auto& w:b.writes) {
            const bool buf=w.descriptorType==vk::DescriptorType::eUniformBuffer || w.descriptorType==vk::DescriptorType::eStorageBuffer;
            const bool img=w.descriptorType==vk::DescriptorType::eSampledImage || w.descriptorType==vk::DescriptorType::eSampler;
            if(w.pNext || w.pTexelBufferView || (buf ? (w.pImageInfo || !DescriptorSnapshot::BufferSliceValid(w.pBufferInfo,w.descriptorCount,buffers)) :
                (!img || w.pBufferInfo || !DescriptorSnapshot::ImageSliceValid(w.pImageInfo,w.descriptorCount,images)))) {candidate->descriptor_structure=false;break;}
            if(build_actions) {
                PreparedDraw::DescriptorAction action{};action.is_buffer=buf;
                if(buf) {
                    action.buffer_first=static_cast<std::size_t>(w.pBufferInfo-b.buffers.data());
                    if(w.descriptorCount==1) {
                        size_t base=0;
                        for(size_t si=0;si<candidate->stages.size();++si) {
                            const auto n=candidate->stages[si].count;
                            if(action.buffer_first>=base && action.buffer_first-base<n) {
                                action.stage_index=static_cast<u32>(si);action.stage_local=action.buffer_first-base;break;
                            }
                            base+=n;
                        }
                        // Indices are hints only; the recorder rechecks every predicate and scans on a miss.
                        if(action.stage_index<stages.size() && stages[action.stage_index] &&
                           action.stage_local<stages[action.stage_index]->buffers.size() &&
                           p->buffer_ranges.size()<=64 && p->regions.size()<=64) {
                            const auto& stage=*stages[action.stage_index];
                            const auto& desc=stage.buffers[action.stage_local];
                            if(!desc.IsSpecial() && !desc.is_written) {
                                const auto sharp=desc.GetSharp(stage);const u64 base=sharp.base_address,bytes=sharp.GetSize();
                                for(size_t i=0;i<p->buffer_ranges.size();++i) {
                                    const auto [begin,end]=p->buffer_ranges[i];
                                    if(begin<=base && end>=base && bytes<=end-base){action.range_hint=static_cast<u8>(i);break;}
                                }
                                for(size_t i=0;i<p->regions.size();++i) {
                                    const auto& use=p->regions[i];
                                    if(use.address==base && use.size==bytes){action.region_hint=static_cast<u8>(i);break;}
                                }
                            }
                        }
                        action.upload_match=0;
                        for(const auto& upload:p->uploads) if(upload.info==action.buffer_first) {
                            if(action.upload_match==1){action.upload_match=2;break;}
                            action.upload_match=1;action.upload_size=upload.size;
                        }
                    }
                }
                p->descriptor_actions.push_back(action);
            }
        }
        if(!candidate->descriptor_structure)p->descriptor_actions.clear();
}catch(const std::bad_alloc&){p->cpu_proof={};p->descriptor_actions.clear();}}
template<class T,class F>bool TimedProofInvoke(T&,bool,F f){return f();}
struct Counters{int proof_phases[6]{};};
bool query(PreparedDraw*prepared,const PreparedDraw::DescriptorAction*action,std::size_t index,const vk::DescriptorBufferInfo*info){Counters c;auto*parallel=&c;bool phase_sample=false;std::size_t action_upload_lookups=0;const bool recorded=TimedProofInvoke(parallel->proof_phases[4],phase_sample,[&] {
                                if(action && action->upload_match!=2) {
                                    ++action_upload_lookups;
                                    return action->upload_match==1 && action->upload_size==info->range;
                                }
                                return std::ranges::any_of(prepared->uploads,[&](const auto& upload){return upload.info==index && upload.size==info->range;});
                            });
return recorded;}
static vk::WriteDescriptorSet row(vk::DescriptorBufferInfo*p,u32 n=1){return {.descriptorCount=n,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=p};}
static bool oracle(const PreparedDraw&p,std::size_t index,u64 range){for(auto&x:p.uploads)if(x.info==index&&x.size==range)return true;return false;}
int main(){PreparedDraw p;p.bindings.buffers.resize(4);for(auto&i:p.bindings.buffers)i.range=12;p.bindings.writes={row(&p.bindings.buffers[0]),row(&p.bindings.buffers[1]),row(&p.bindings.buffers[2],2)};p.uploads={{0,12},{1,8},{1,12},{2,12}};produce(&p);assert(p.cpu_proof.descriptor_structure&&p.descriptor_actions.size()==3);assert(p.descriptor_actions[0].upload_match==1&&p.descriptor_actions[1].upload_match==2&&p.descriptor_actions[2].upload_match==2);for(size_t i=0;i<4;++i){auto&a=p.descriptor_actions[i<2?i:2];for(u64 range:{0ull,8ull,12ull,13ull}){p.bindings.buffers[i].range=range;assert(query(&p,&a,i,&p.bindings.buffers[i])==oracle(p,i,range));assert(query(&p,nullptr,i,&p.bindings.buffers[i])==oracle(p,i,range));}}
 p.bindings.writes={row(&p.bindings.buffers[3])};produce(&p);assert(p.descriptor_actions[0].upload_match==0&&!query(&p,&p.descriptor_actions[0],3,&p.bindings.buffers[3]));auto cap=p.descriptor_actions.capacity();p.descriptor_actions.clear();assert(p.descriptor_actions.capacity()==cap);
 p.bindings.writes.assign(128,row(&p.bindings.buffers[0]));produce(&p);assert(p.cpu_proof.descriptor_structure&&p.descriptor_actions.size()==128);p.bindings.writes.push_back(row(&p.bindings.buffers[0]));produce(&p);assert(p.cpu_proof.descriptor_structure&&p.descriptor_actions.empty());
 p.bindings.writes={row(nullptr)};produce(&p);assert(!p.cpu_proof.descriptor_structure&&p.descriptor_actions.empty());PreparedDraw oom;oom.bindings.buffers.resize(1);oom.bindings.writes.assign(128,row(&oom.bindings.buffers[0]));fail_alloc=true;produce(&oom);assert(!fail_alloc&&!oom.cpu_proof.descriptor_structure&&oom.descriptor_actions.empty());puts("PASS exact producer/upload lookup: no upload, unique, duplicates, multi, range mutation, stale fallback, 128/129, malformed, reuse, OOM");}
