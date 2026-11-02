// SPDX-License-Identifier: GPL-2.0-or-later
// Uses mocked resource types; producer block must match renderer source.
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <memory>
#include <span>
#include <vector>
#include <array>
#include <optional>
#include <cstdio>
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using u64=uint64_t;using std::size_t;
namespace Shader {constexpr u32 MaxStageTypes=6; enum class BufferType { Other, DirectTable };struct Buffer {BufferType buffer_type{}; bool is_written{}; bool IsSpecial()const{return false;} struct Sharp {u64 base_address{};u64 GetSize()const{return 0;}}; Sharp GetSharp(const auto&)const{return {};} };struct Info {u64 identity{};std::vector<Buffer> buffers;};}
static bool fail_new=false;void* operator new(size_t n){if(fail_new){fail_new=false;throw std::bad_alloc();}if(auto p=malloc(n))return p;throw std::bad_alloc();}void operator delete(void*p) noexcept{free(p);}void operator delete(void*p,size_t) noexcept{free(p);}
namespace vk{enum class DescriptorType{eUniformBuffer,eStorageBuffer,eSampledImage,eSampler};struct DescriptorBufferInfo{int x;};struct DescriptorImageInfo{int x;};struct WriteDescriptorSet{DescriptorType descriptorType{};u32 descriptorCount{};const void*pNext{},*pTexelBufferView{};const DescriptorBufferInfo*pBufferInfo{};const DescriptorImageInfo*pImageInfo{};};}
struct DescriptorSnapshot{template<class T>static bool Valid(const T*p,size_t n,std::span<const T>x){return n&&p==x.data()&&n<=x.size();}static bool BufferSliceValid(const vk::DescriptorBufferInfo*p,size_t n,std::span<const vk::DescriptorBufferInfo>x){return Valid(p,n,x);}static bool ImageSliceValid(const vk::DescriptorImageInfo*p,size_t n,std::span<const vk::DescriptorImageInfo>x){return Valid(p,n,x);}static std::optional<DescriptorSnapshot> Make(auto,auto,auto,size_t){return DescriptorSnapshot{};}};
struct Bind{u32 binding;const void*pNext;};struct Vertex{std::vector<int> buffers,offsets,sizes,strides;std::vector<Bind> input_bindings,attributes;};
struct Proof{struct StageSchema{u64 identity{};size_t count{},table_local{SIZE_MAX},table_upload{SIZE_MAX};};std::array<StageSchema,5> stages{};const void*prepared_identity{},*pipeline_identity{},*writes_data{},*buffers_data{},*images_data{},*vertex_identity{},*vertex_buffers_data{},*push_data{};size_t writes_size{},buffers_size{},images_size{},vertex_buffers_size{},push_size{},push_buffer_first{},push_buffer_count{};int pipeline_layout{};bool descriptor_structure{},vertex_shape{},vertex_bindings{},vertex_pnext{};std::array<const void*,5>group_data{};std::array<size_t,5>group_sizes{};size_t ordinary_writes{},ordinary_buffers{},ordinary_images{};u32 counts_push{};bool counts_valid{};u32 push_index{};std::shared_ptr<void> push_snapshot;};
struct B{std::vector<vk::WriteDescriptorSet>writes;std::vector<vk::DescriptorBufferInfo>buffers;std::vector<vk::DescriptorImageInfo>images;std::array<std::vector<vk::WriteDescriptorSet>,1>grouped_writes;};struct PreparedDraw{struct DescriptorAction{u32 upload_size{};u16 buffer_first{},stage_local{};u8 range_hint{255},region_hint{255};u8 stage_index{255},upload_match{2};bool is_buffer{};};struct Upload{u32 info{},size{};};Proof cpu_proof;B bindings;std::vector<DescriptorAction> descriptor_actions;std::vector<Upload> uploads;std::vector<std::pair<u64,u64>>buffer_ranges;struct Region{u64 address,size;};std::vector<Region>regions;bool has_vertices{};std::unique_ptr<Vertex>vertex_index;};using P=PreparedDraw;struct Pipeline{int GetLayout()const{return 4;}u32 PushSet()const{return 0;}};bool ParallelRecordingRequested(){return true;}
void run(P*p,Pipeline&pipeline){std::array<const Shader::Info*,5> stages{};if(ParallelRecordingRequested())try{
        auto* candidate=&p->cpu_proof;candidate->prepared_identity=p;candidate->pipeline_identity=&pipeline;
        candidate->pipeline_layout=pipeline.GetLayout();
        const auto& b=p->bindings;candidate->writes_data=b.writes.data();candidate->writes_size=b.writes.size();
        candidate->buffers_data=b.buffers.data();candidate->buffers_size=b.buffers.size();candidate->images_data=b.images.data();candidate->images_size=b.images.size();
        const auto buffers=std::span<const vk::DescriptorBufferInfo>{b.buffers.data(),b.buffers.size()};
        const auto images=std::span<const vk::DescriptorImageInfo>{b.images.data(),b.images.size()};
        candidate->descriptor_structure=true;
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
        static_assert(Shader::MaxStageTypes-1<255);
        const bool build_actions=candidate->descriptor_structure && b.writes.size()<=128 &&
            b.buffers.size()<=UINT16_MAX;
        if(build_actions)p->descriptor_actions.reserve(b.writes.size());
        for(const auto& w:b.writes) {
            const bool buf=w.descriptorType==vk::DescriptorType::eUniformBuffer || w.descriptorType==vk::DescriptorType::eStorageBuffer;
            const bool img=w.descriptorType==vk::DescriptorType::eSampledImage || w.descriptorType==vk::DescriptorType::eSampler;
            if(w.pNext || w.pTexelBufferView || (buf ? (w.pImageInfo || !DescriptorSnapshot::BufferSliceValid(w.pBufferInfo,w.descriptorCount,buffers)) :
                (!img || w.pBufferInfo || !DescriptorSnapshot::ImageSliceValid(w.pImageInfo,w.descriptorCount,images)))) {candidate->descriptor_structure=false;break;}
            if(build_actions) {
                PreparedDraw::DescriptorAction action{};action.is_buffer=buf;
                if(buf) {
                    action.buffer_first=static_cast<u16>(w.pBufferInfo-b.buffers.data());
                    if(w.descriptorCount==1) {
                        size_t base=0;
                        for(size_t si=0;si<candidate->stages.size();++si) {
                            const auto n=candidate->stages[si].count;
                            if(action.buffer_first>=base && action.buffer_first-base<n) {
                                action.stage_index=static_cast<u8>(si);action.stage_local=static_cast<u16>(action.buffer_first-base);break;
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
        if(p->has_vertices && p->vertex_index) {
            const auto& v=*p->vertex_index;candidate->vertex_identity=&v;candidate->vertex_buffers_data=v.buffers.data();candidate->vertex_buffers_size=v.buffers.size();
            const size_t n=v.buffers.size();candidate->vertex_shape=n && n<=32 && v.input_bindings.size()==n && v.attributes.size()==n && v.offsets.size()==n && v.sizes.size()==n && v.strides.size()==n;
            if(candidate->vertex_shape) {
                candidate->vertex_bindings=true;candidate->vertex_pnext=true;
                for(size_t i=0;i<n;++i)if(v.input_bindings[i].binding!=i || v.attributes[i].binding!=i)candidate->vertex_bindings=false;
                for(const auto& x:v.input_bindings)if(x.pNext)candidate->vertex_pnext=false;
                for(const auto& x:v.attributes)if(x.pNext)candidate->vertex_pnext=false;
            }
        }
        candidate->counts_valid=candidate->descriptor_structure;
        candidate->counts_push=pipeline.PushSet();
        for(size_t set=0;candidate->counts_valid && set<b.grouped_writes.size();++set) {
            const auto group=b.grouped_writes[set];candidate->group_data[set]=group.data();candidate->group_sizes[set]=group.size();
            if(set==candidate->counts_push)continue;
            for(const auto& row:group) {
                auto& count=row.pBufferInfo ? candidate->ordinary_buffers : candidate->ordinary_images;
                if(candidate->ordinary_writes==SIZE_MAX || row.descriptorCount>SIZE_MAX-count) {candidate->counts_valid=false;break;}
                ++candidate->ordinary_writes;count+=row.descriptorCount;
            }
        }
        const u32 push=pipeline.PushSet();
        if(candidate->descriptor_structure && push<b.grouped_writes.size() && !b.grouped_writes[push].empty()) {
            const auto& group=b.grouped_writes[push];bool contiguous=true,has_buffer=false;size_t first=0,count=0;
            for(const auto& w:group) if(w.pBufferInfo) {
                // descriptor_structure validated this exact slice before pointer subtraction.
                const size_t offset=static_cast<size_t>(w.pBufferInfo-b.buffers.data());
                if(!has_buffer){first=offset;has_buffer=true;}
                if(offset!=first+count || w.descriptorCount>b.buffers.size()-offset){contiguous=false;break;}
                count+=w.descriptorCount;
            }
            if(contiguous)if(auto snap=DescriptorSnapshot::Make(group,buffers,images,64*1024)) {
                candidate->push_index=push;candidate->push_data=group.data();candidate->push_size=group.size();
                candidate->push_buffer_first=first;candidate->push_buffer_count=count;
                candidate->push_snapshot=std::make_shared<DescriptorSnapshot>(std::move(*snap));
            }
        }
    } catch(const std::bad_alloc&) {p->cpu_proof = {};p->descriptor_actions.clear();}

}
int main(){P x;Pipeline pipeline;x.bindings.images.resize(1);vk::WriteDescriptorSet w{};w.descriptorType=vk::DescriptorType::eSampledImage;w.descriptorCount=1;w.pImageInfo=x.bindings.images.data();x.bindings.writes.push_back(w);x.bindings.grouped_writes[0].push_back(w);fail_new=true;run(&x,pipeline);assert(!fail_new&&!x.cpu_proof.prepared_identity&&!x.cpu_proof.push_snapshot&&!x.cpu_proof.descriptor_structure);run(&x,pipeline);assert(x.cpu_proof.prepared_identity==&x && x.cpu_proof.descriptor_structure && x.cpu_proof.push_snapshot);puts("PASS exact producer block OOM resets proof and later rebuild succeeds");}
