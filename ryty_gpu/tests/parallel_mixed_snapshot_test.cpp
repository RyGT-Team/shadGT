// SPDX-License-Identifier: GPL-2.0-or-later
// Producer and consumer fragments are checked against the renderer source by
// verify_parallel_snapshot_fragments.py.
#include "ryty_gpu/native/descriptor_snapshot.h"
#include <cassert>
#include <array>
#include <vector>
#include <memory>
#include <cstdio>
using u32=uint32_t;using std::size_t;
struct B{std::vector<vk::DescriptorBufferInfo> buffers;std::vector<vk::DescriptorImageInfo> images;std::array<std::vector<vk::WriteDescriptorSet>,1> grouped_writes;};
struct Proof{bool descriptor_structure=true;u32 push_index{};const void*push_data{};size_t push_size{},push_buffer_first{},push_buffer_count{};std::shared_ptr<void>push_snapshot;};struct Pipeline{u32 PushSet(){return 0;}};
void produce(B&b,Proof&proof){auto*candidate=&proof;Pipeline pipeline;const auto buffers=std::span<const vk::DescriptorBufferInfo>{b.buffers.data(),b.buffers.size()};const auto images=std::span<const vk::DescriptorImageInfo>{b.images.data(),b.images.size()};
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
        }}
std::shared_ptr<const DescriptorSnapshot> consume(B&b,Proof&proof,int&hits,bool valid=true,bool feedback_value=false){auto*worker_result=&proof;bool worker_identity=valid,known_structure=true,descriptor_safe=true;std::vector<int>feedback;if(feedback_value)feedback.push_back(1);u32 push=0;constexpr size_t MaxSnapshotBytes=64*1024;struct Counters{int producer_snapshot_hits{};} c;auto*parallel=&c;int descriptor_fail_reason=0;size_t parallel_descriptor_bytes=0;std::shared_ptr<const DescriptorSnapshot>parallel_push_writes;
const auto snapshot_matches=[&] {
                            if(!worker_identity || !known_structure || !feedback.empty() || !worker_result->push_snapshot || worker_result->push_index!=push ||
                               worker_result->push_data!=b.grouped_writes[push].data() || worker_result->push_size!=b.grouped_writes[push].size()) return false;
                            // Producer group rows and image values remain immutable after publication; feedback is excluded above.
                            return true;
                        };
                        bool reused_snapshot=false;
                        if(snapshot_matches() && worker_result->push_snapshot.use_count()==1 &&
                           worker_result->push_buffer_first<=b.buffers.size() &&
                           worker_result->push_buffer_count<=b.buffers.size()-worker_result->push_buffer_first) {
                            auto owned=std::static_pointer_cast<DescriptorSnapshot>(std::move(worker_result->push_snapshot));
                            const auto current=std::span<const vk::DescriptorBufferInfo>{b.buffers.data(),b.buffers.size()}.subspan(worker_result->push_buffer_first,worker_result->push_buffer_count);
                            if(owned->PatchBufferBindings(current)) {
                                parallel_push_writes=std::move(owned);NATIVE_DIAGNOSTIC(++parallel->producer_snapshot_hits);
                                parallel_descriptor_bytes=parallel_push_writes->OwnedBytes()+128;reused_snapshot=true;
                            }
                        }
                        if(!reused_snapshot) {
                            auto snap=DescriptorSnapshot::Make(b.grouped_writes[push],
                                std::span<const vk::DescriptorBufferInfo>{b.buffers.data(),b.buffers.size()},
                                std::span<const vk::DescriptorImageInfo>{b.images.data(),b.images.size()},MaxSnapshotBytes);
                            if(!snap){descriptor_fail_reason=9;descriptor_safe=false;}
                            else {parallel_descriptor_bytes=snap->OwnedBytes()+128;parallel_push_writes=std::make_shared<const DescriptorSnapshot>(std::move(*snap));}
                        }hits+=c.producer_snapshot_hits;return parallel_push_writes;}
static vk::Buffer buf(uintptr_t n){return vk::Buffer{reinterpret_cast<VkBuffer>(n)};}
int main(){using namespace vk;B b;b.buffers={DescriptorBufferInfo{buf(1),0,8},DescriptorBufferInfo{buf(2),8,8},DescriptorBufferInfo{buf(3),16,8}};b.images={{Sampler{},ImageView{reinterpret_cast<VkImageView>(7)},ImageLayout::eGeneral}};auto&g=b.grouped_writes[0];g={{.dstBinding=0,.descriptorCount=1,.descriptorType=DescriptorType::eStorageBuffer,.pBufferInfo=&b.buffers[0]},{.dstBinding=1,.descriptorCount=1,.descriptorType=DescriptorType::eSampledImage,.pImageInfo=&b.images[0]},{.dstBinding=2,.descriptorCount=1,.descriptorType=DescriptorType::eStorageBuffer,.pBufferInfo=&b.buffers[1]}};
 int hits=0;Proof a;produce(b,a);assert(a.push_snapshot && a.push_buffer_first==0 && a.push_buffer_count==2 && a.push_snapshot.use_count()==1);b.buffers[0].buffer=buf(10);b.buffers[1].buffer=buf(11);auto first=consume(b,a,hits);assert(first&&hits==1&&!a.push_snapshot);assert(first->Writes()[0].pBufferInfo->buffer==buf(10)&&first->Writes()[2].pBufferInfo->buffer==buf(11));
 Proof second;produce(b,second);b.buffers[0].buffer=buf(20);b.buffers[1].buffer=buf(21);auto second_draw=consume(b,second,hits);assert(hits==2&&second_draw->Writes()[0].pBufferInfo->buffer==buf(20)&&first->Writes()[0].pBufferInfo->buffer==buf(10));
 Proof shared;produce(b,shared);auto alias=shared.push_snapshot;auto fallback=consume(b,shared,hits);assert(fallback&&hits==2&&shared.push_snapshot&&alias.use_count()==2);
 Proof mismatch;produce(b,mismatch);auto before=std::static_pointer_cast<DescriptorSnapshot>(mismatch.push_snapshot)->Writes()[0].pBufferInfo->buffer;mismatch.push_buffer_count=1;auto fb=consume(b,mismatch,hits);assert(fb&&hits==2&&!mismatch.push_snapshot&&before==buf(20));
 g[2].pBufferInfo=&b.buffers[2];Proof gap;produce(b,gap);assert(!gap.push_snapshot);g[2].pBufferInfo=&b.buffers[0];Proof overlap;produce(b,overlap);assert(!overlap.push_snapshot);
 g[0].pBufferInfo=&b.buffers[1];g[2].pBufferInfo=&b.buffers[0];Proof reorder;produce(b,reorder);assert(!reorder.push_snapshot);
 g[0].pBufferInfo=&b.buffers[0];g[0].descriptorCount=2;g.erase(g.begin()+2);Proof multi;produce(b,multi);assert(multi.push_snapshot&&multi.push_buffer_count==2);
 Proof feedback;produce(b,feedback);const int before_feedback=hits;auto feedback_draw=consume(b,feedback,hits,true,true);assert(feedback_draw&&hits==before_feedback&&feedback.push_snapshot);
 Proof stale;produce(b,stale);auto stale_draw=consume(b,stale,hits,false,false);assert(stale_draw&&hits==before_feedback&&stale.push_snapshot);
 puts("PASS exact mixed producer/consumer: unique branch, latest buffer, queued immutability, shared/mismatch fallback, gap/overlap/reorder and multi");}
