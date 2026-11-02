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
using u32=uint32_t;using std::size_t;
static bool fail_new=false;void* operator new(size_t n){if(fail_new){fail_new=false;throw std::bad_alloc();}if(auto p=malloc(n))return p;throw std::bad_alloc();}void operator delete(void*p) noexcept{free(p);}void operator delete(void*p,size_t) noexcept{free(p);}
namespace vk{enum class DescriptorType{eUniformBuffer,eStorageBuffer,eSampledImage,eSampler};struct DescriptorBufferInfo{int x;};struct DescriptorImageInfo{int x;};struct WriteDescriptorSet{DescriptorType descriptorType{};u32 descriptorCount{};const void*pNext{},*pTexelBufferView{};const DescriptorBufferInfo*pBufferInfo{};const DescriptorImageInfo*pImageInfo{};};}
struct DescriptorSnapshot{template<class T>static bool Valid(const T*p,size_t n,std::span<const T>x){return n&&p==x.data()&&n<=x.size();}static bool BufferSliceValid(const vk::DescriptorBufferInfo*p,size_t n,std::span<const vk::DescriptorBufferInfo>x){return Valid(p,n,x);}static bool ImageSliceValid(const vk::DescriptorImageInfo*p,size_t n,std::span<const vk::DescriptorImageInfo>x){return Valid(p,n,x);}static std::optional<DescriptorSnapshot> Make(auto,auto,auto,size_t){return DescriptorSnapshot{};}};
struct Bind{u32 binding;const void*pNext;};struct Vertex{std::vector<int> buffers,offsets,sizes,strides;std::vector<Bind> input_bindings,attributes;};
struct Proof{const void*prepared_identity{},*pipeline_identity{},*writes_data{},*buffers_data{},*images_data{},*vertex_identity{},*vertex_buffers_data{},*push_data{};size_t writes_size{},buffers_size{},images_size{},vertex_buffers_size{},push_size{},push_buffer_first{},push_buffer_count{};int pipeline_layout{};bool descriptor_structure{},vertex_shape{},vertex_bindings{},vertex_pnext{};u32 push_index{};std::shared_ptr<void> push_snapshot;};
struct B{std::vector<vk::WriteDescriptorSet>writes;std::vector<vk::DescriptorBufferInfo>buffers;std::vector<vk::DescriptorImageInfo>images;std::array<std::vector<vk::WriteDescriptorSet>,1>grouped_writes;};struct P{Proof cpu_proof;B bindings;bool has_vertices{};std::unique_ptr<Vertex>vertex_index;};struct Pipeline{int GetLayout()const{return 4;}u32 PushSet()const{return 0;}};bool ParallelRecordingRequested(){return true;}
void run(P*p,Pipeline&pipeline){if(ParallelRecordingRequested())try{

        auto* candidate=&p->cpu_proof;candidate->prepared_identity=p;candidate->pipeline_identity=&pipeline;
        candidate->pipeline_layout=pipeline.GetLayout();
        const auto& b=p->bindings;candidate->writes_data=b.writes.data();candidate->writes_size=b.writes.size();
        candidate->buffers_data=b.buffers.data();candidate->buffers_size=b.buffers.size();candidate->images_data=b.images.data();candidate->images_size=b.images.size();
        const auto buffers=std::span<const vk::DescriptorBufferInfo>{b.buffers.data(),b.buffers.size()};
        const auto images=std::span<const vk::DescriptorImageInfo>{b.images.data(),b.images.size()};
        candidate->descriptor_structure=true;
        for(const auto& w:b.writes) {
            const bool buf=w.descriptorType==vk::DescriptorType::eUniformBuffer || w.descriptorType==vk::DescriptorType::eStorageBuffer;
            const bool img=w.descriptorType==vk::DescriptorType::eSampledImage || w.descriptorType==vk::DescriptorType::eSampler;
            if(w.pNext || w.pTexelBufferView || (buf ? (w.pImageInfo || !DescriptorSnapshot::BufferSliceValid(w.pBufferInfo,w.descriptorCount,buffers)) :
                (!img || w.pBufferInfo || !DescriptorSnapshot::ImageSliceValid(w.pImageInfo,w.descriptorCount,images)))) {candidate->descriptor_structure=false;break;}
        }
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
    } catch(const std::bad_alloc&) {p->cpu_proof = {};}
}
int main(){P x;Pipeline pipeline;x.bindings.images.resize(1);vk::WriteDescriptorSet w{};w.descriptorType=vk::DescriptorType::eSampledImage;w.descriptorCount=1;w.pImageInfo=x.bindings.images.data();x.bindings.writes.push_back(w);x.bindings.grouped_writes[0].push_back(w);fail_new=true;run(&x,pipeline);assert(!fail_new&&!x.cpu_proof.prepared_identity&&!x.cpu_proof.push_snapshot&&!x.cpu_proof.descriptor_structure);run(&x,pipeline);assert(x.cpu_proof.prepared_identity==&x && x.cpu_proof.descriptor_structure && x.cpu_proof.push_snapshot);puts("PASS exact producer block OOM resets proof and later rebuild succeeds");}
