#include "ryty_gpu/native/parallel_pass_recording.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_command_stream.h"
#include "video_core/renderer_vulkan/vertex_input_equal.h"
#include <bit>
#if !defined(NDEBUG)
#include <chrono>
#include <cstdlib>
#include <mutex>
#endif
namespace Native::Parallel {
#if !defined(NDEBUG)
namespace {
using Clock = std::chrono::steady_clock;
std::mutex recording_probe_mutex;
RecordingCostProbes recording_probes{};
bool RecordingProbeEnabled() {
    static const bool enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_GATE_DIAG");return p && p[0]=='1' && !p[1];}();
    return enabled;
}
struct PassProbe {
    bool enabled=RecordingProbeEnabled();
    bool failed=true;
    Clock::time_point start=enabled ? Clock::now() : Clock::time_point{};
    RecordingCostProbes local{};
    Clock::time_point Start() const {return enabled ? Clock::now() : Clock::time_point{};}
    void Add(u64& field,Clock::time_point begin) const {
        if(enabled) field+=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count();
    }
    ~PassProbe() {
        if(!enabled)return;
        Add(local.total_pass_ns,start); ++local.pass_count; if(failed)++local.failure_count;
        std::lock_guard lock(recording_probe_mutex);
#define MERGE(field) recording_probes.field+=local.field
        MERGE(validate_ns); MERGE(pipeline_bind_ns); MERGE(vertex_bind_ns); MERGE(descriptor_bind_ns);
        MERGE(draw_call_ns); MERGE(draw_count); MERGE(pipeline_change_count); MERGE(vertex_change_count);
        MERGE(descriptor_flush_count); MERGE(total_pass_ns); MERGE(pass_count); MERGE(failure_count);
#undef MERGE
    }
};
}
#endif
static bool ValidatePassHeader(const Pass& pass) {
    const auto& target=pass.target;
    if(target.num_color_attachments>8 || !target.width || !target.height || !target.num_layers ||
       !pass.resource_lease) return false;
    if(!pass.extended_verified && (target.num_color_attachments!=1 || !target.color_attachments[0].image_view ||
       target.color_attachments[0].is_clear || target.depth_stencil_attachment.has_depth || target.depth_stencil_attachment.has_stencil))return false;
    if(pass.extended_verified && (target.depth_stencil_attachment.has_depth || target.depth_stencil_attachment.has_stencil) && !target.depth_stencil_attachment.image_view)return false;
    return true;
}
static bool ValidateDraw(const Draw& draw,bool extended_verified) {
        if(!draw.pipeline || !draw.layout || !draw.pipeline_owner || draw.push_set>draw.sets.size())return false;
        if(draw.indexed) {
            if(!extended_verified || !draw.index_backing_verified || !draw.index_buffer ||
               (draw.index_type!=vk::IndexType::eUint16 && draw.index_type!=vk::IndexType::eUint32))return false;
            const u64 element=draw.index_type==vk::IndexType::eUint16?2:4;
            if(draw.index_offset%element || !draw.index_size || draw.index_offset>~vk::DeviceSize{0}-draw.index_size || u64(draw.vertices)>draw.index_size/element)return false;
        }
        if(draw.vertex_attribute_count>32 || draw.vertex_binding_count>32 || draw.vertex_buffer_count>32)return false;
        if(draw.vertex_attributes.size()!=draw.vertex_attribute_count || draw.vertex_bindings.size()!=draw.vertex_binding_count ||
           draw.vertex_buffers.size()!=draw.vertex_buffer_count || draw.vertex_offsets.size()!=draw.vertex_buffer_count ||
           draw.vertex_sizes.size()!=draw.vertex_buffer_count || draw.vertex_strides.size()!=draw.vertex_buffer_count)return false;
        if(draw.vertex_buffer_count!=draw.vertex_binding_count || draw.vertex_buffer_count!=draw.vertex_attribute_count)return false;
        if(draw.vertex_buffer_count && !draw.vertex_backing_verified)return false;
        for(u32 i=0;i<draw.vertex_buffer_count;++i) {
            if(draw.vertex_bindings[i].binding!=i || draw.vertex_attributes[i].binding!=i ||
               !draw.vertex_buffers[i] || !draw.vertex_sizes[i] ||
               draw.vertex_attributes[i].pNext || draw.vertex_bindings[i].pNext)return false;
        }
        static_assert(Shader::Backend::MaxDescriptorSets<=32);
        for(u32 i=0;i<draw.sets.size();++i) if((draw.required_sets_mask & (u32{1}<<i)) && !draw.sets[i])return false;
        if(draw.push_set<draw.sets.size() && (draw.required_sets_mask & (u32{1}<<draw.push_set)))return false;
        if(draw.push_writes) {
            if(draw.push_set>=draw.sets.size() || draw.sets[draw.push_set])return false;
            if(!draw.push_writes->ParallelReplaySafe(draw.immutable_upload_buffers_verified))return false;
        }
    return true;
}
bool ValidatePass(const Pass& pass) {
    if(!ValidatePassHeader(pass) || pass.draws.empty() || pass.draws.size()>256)return false;
    for(const auto& draw:pass.draws)if(!ValidateDraw(draw,pass.extended_verified))return false;
    return true;
}
struct PassTraversal {
    const Pass& pass;
    DrawPublication* publication;
    const Draw* storage;
#if !defined(NDEBUG)
    std::size_t* early_draws;
#endif
    bool valid=true;
};
template<class Begin,class Emit,class End>
static void EmitWholePass(PassTraversal& traversal,Begin&& begin,Emit&& emit,End&& end) {
    if(!traversal.publication) {
        EmitWholePass(traversal.pass,begin,emit,end);
        return;
    }
    begin(traversal.pass.target);
    std::size_t consumed=0;
    for(;;) {
        const auto available=traversal.publication->Wait(consumed);
        for(;consumed<available.count;++consumed) {
            const auto& draw=traversal.storage[consumed];
            if(!ValidateDraw(draw,traversal.pass.extended_verified)) {
                traversal.valid=false;end();return;
            }
            emit(draw);
#if !defined(NDEBUG)
            if(traversal.early_draws && !traversal.publication->Load().sealed)++*traversal.early_draws;
#endif
        }
        if(available.sealed)break;
    }
    end();
}
static bool RecordPassImpl(vk::CommandBuffer cmd,const Vulkan::Instance& instance,const Pass& pass,
                           Vulkan::DynamicState* inline_dynamic,vk::Pipeline* inline_pipeline,
                           DrawPublication* publication=nullptr,const Draw* storage=nullptr
#if !defined(NDEBUG)
                           ,std::size_t* early_draws=nullptr
#endif
                           ) {
#if !defined(NDEBUG)
    PassProbe probe;
    const auto t_validate_start = probe.Start();
#endif
    const bool valid=cmd && (publication ? storage && ValidatePassHeader(pass) : ValidatePass(pass));
#if !defined(NDEBUG)
    probe.Add(probe.local.validate_ns,t_validate_start);
#endif
    if(!valid) return false;
    const auto& target=pass.target;
    std::array<vk::RenderingAttachmentInfo,8> colors{};
    for(u32 i=0;i<target.num_color_attachments;++i) {const auto& cb=target.color_attachments[i];colors[i]=vk::RenderingAttachmentInfo{
        .imageView=cb.image_view,.imageLayout=cb.image_layout,.loadOp=cb.is_clear?vk::AttachmentLoadOp::eClear:vk::AttachmentLoadOp::eLoad,
        .storeOp=vk::AttachmentStoreOp::eStore,.clearValue=vk::ClearValue{.color=vk::ClearColorValue{.uint32=cb.clear_value}}};}
    const auto& db=target.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth{.imageView=db.image_view,.imageLayout=vk::ImageLayout::eGeneral,
        .loadOp=db.depth_clear?vk::AttachmentLoadOp::eClear:vk::AttachmentLoadOp::eLoad,.storeOp=vk::AttachmentStoreOp::eStore,
        .clearValue=vk::ClearValue{.depthStencil=vk::ClearDepthStencilValue{.depth=std::bit_cast<float>(db.clear_value[0])}}};
    const vk::RenderingAttachmentInfo stencil{.imageView=db.image_view,.imageLayout=vk::ImageLayout::eGeneral,
        .loadOp=db.stencil_clear?vk::AttachmentLoadOp::eClear:vk::AttachmentLoadOp::eLoad,.storeOp=vk::AttachmentStoreOp::eStore,
        .clearValue=vk::ClearValue{.depthStencil=vk::ClearDepthStencilValue{.stencil=db.clear_value[1]}}};
    const Vulkan::CmdBuf commands{cmd};
    const auto flags=vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
        vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry | vk::ShaderStageFlagBits::eFragment;
    Vulkan::DynamicState local_dynamic{};
    auto& dynamic=inline_dynamic?*inline_dynamic:local_dynamic;
    dynamic.Invalidate();
    vk::Pipeline bound_pipeline{};
    const Draw* bound_vertex = nullptr;
    PassTraversal traversal{pass,publication,storage
#if !defined(NDEBUG)
        ,early_draws
#endif
        };
    const auto same_vertex = [](const Draw& a, const Draw& b) {
        return a.pipeline == b.pipeline &&
               Vulkan::VertexInputBytesEqual(a.vertex_attributes, b.vertex_attributes) &&
               Vulkan::VertexInputBytesEqual(a.vertex_bindings, b.vertex_bindings) &&
               Vulkan::VertexInputBytesEqual(a.vertex_buffers, b.vertex_buffers) &&
               Vulkan::VertexInputBytesEqual(a.vertex_offsets, b.vertex_offsets) &&
               Vulkan::VertexInputBytesEqual(a.vertex_sizes, b.vertex_sizes) &&
               Vulkan::VertexInputBytesEqual(a.vertex_strides, b.vertex_strides);
    };
    EmitWholePass(traversal,
        [&](const auto&) {cmd.beginRendering(vk::RenderingInfo{.renderArea{.offset={0,0},.extent={target.width,target.height}},
            .layerCount=target.num_layers,.colorAttachmentCount=target.num_color_attachments,.pColorAttachments=colors.data(),
            .pDepthAttachment=db.has_depth?&depth:nullptr,.pStencilAttachment=db.has_stencil?&stencil:nullptr});},
        [&](const Draw& draw) {
            ApplyDynamicSnapshot(dynamic,draw.dynamic);dynamic.Commit(instance,commands);
            if (!bound_vertex || !same_vertex(*bound_vertex, draw)) {
#if !defined(NDEBUG)
                const auto t_vertex_start = probe.Start();
#endif
                if(instance.IsVertexInputDynamicState()) {
                    commands.setVertexInputEXT(vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT>{draw.vertex_binding_count,draw.vertex_bindings.data()},
                        vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT>{draw.vertex_attribute_count,draw.vertex_attributes.data()});
                    if(draw.vertex_buffer_count) commands.bindVertexBuffers(0,draw.vertex_buffer_count,draw.vertex_buffers.data(),draw.vertex_offsets.data());
                } else if(draw.vertex_buffer_count) {
                    commands.bindVertexBuffers2(0,draw.vertex_buffer_count,draw.vertex_buffers.data(),draw.vertex_offsets.data(),draw.vertex_sizes.data(),draw.vertex_strides.data());
                }
                bound_vertex = &draw;
#if !defined(NDEBUG)
                probe.Add(probe.local.vertex_bind_ns,t_vertex_start);
                if(probe.enabled) ++probe.local.vertex_change_count;
#endif
            }
            if(bound_pipeline!=draw.pipeline) {
#if !defined(NDEBUG)
                const auto t_pipeline_start = probe.Start();
#endif
                commands.bindPipeline(vk::PipelineBindPoint::eGraphics,draw.pipeline);bound_pipeline=draw.pipeline;
#if !defined(NDEBUG)
                probe.Add(probe.local.pipeline_bind_ns,t_pipeline_start);
                if(probe.enabled) ++probe.local.pipeline_change_count;
#endif
            }
            commands.pushConstants(draw.layout,flags,0u,sizeof(draw.push),&draw.push);
            std::array<vk::DescriptorSet,Shader::Backend::MaxDescriptorSets> run{};u32 first=0,count=0;
#if !defined(NDEBUG)
            const auto t_desc_start=probe.Start();
#endif
            const auto flush=[&] {if(count){commands.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,draw.layout,first,
                vk::ArrayProxy<const vk::DescriptorSet>{count,run.data()},{});count=0;
#if !defined(NDEBUG)
                if(probe.enabled) ++probe.local.descriptor_flush_count;
#endif
            }};
            for(u32 i=0;i<draw.sets.size();++i) {
                if(i==draw.push_set && draw.push_writes && !draw.push_writes->Writes().empty()) {
                    flush();const auto writes=draw.push_writes->Writes();
                    commands.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics,draw.layout,i,
                        vk::ArrayProxy<const vk::WriteDescriptorSet>{static_cast<u32>(writes.size()),writes.data()});
                } else if(draw.sets[i]) {if(count && i!=first+count)flush();if(!count)first=i;run[count++]=draw.sets[i];}
            }
            flush();
#if !defined(NDEBUG)
            probe.Add(probe.local.descriptor_bind_ns,t_desc_start);
#endif
            if(draw.indexed) {commands.bindIndexBuffer(draw.index_buffer,draw.index_offset,draw.index_type);
#if !defined(NDEBUG)
                const auto t_draw_start = probe.Start();
#endif
                commands.drawIndexed(draw.vertices,draw.instances,0,draw.vertex_offset,draw.first_instance);
#if !defined(NDEBUG)
                probe.Add(probe.local.draw_call_ns,t_draw_start);
                if(probe.enabled) ++probe.local.draw_count;
#endif
            } else {
#if !defined(NDEBUG)
                const auto t_draw_start = probe.Start();
#endif
                commands.draw(draw.vertices,draw.instances,draw.first_vertex,draw.first_instance);
#if !defined(NDEBUG)
                probe.Add(probe.local.draw_call_ns,t_draw_start);
                if(probe.enabled) ++probe.local.draw_count;
#endif
            }
        },
        [&] {if(!inline_dynamic)cmd.endRendering();});
    if(!traversal.valid)return false;
    if(inline_pipeline)*inline_pipeline=bound_pipeline;
#if !defined(NDEBUG)
    probe.failed=false;
#endif
    return true;
}
bool RecordPass(vk::CommandBuffer cmd,const Vulkan::Instance& instance,const Pass& pass) {
    return RecordPassImpl(cmd,instance,pass,nullptr,nullptr);
}
bool RecordPublishedPass(vk::CommandBuffer cmd,const Vulkan::Instance& instance,
                         const Pass& header,const Draw* storage,DrawPublication& publication
#if !defined(NDEBUG)
                         ,std::size_t* early_draws
#endif
                         ) {
    return RecordPassImpl(cmd,instance,header,nullptr,nullptr,&publication,storage
#if !defined(NDEBUG)
                          ,early_draws
#endif
                          );
}
bool RecordPassInline(vk::CommandBuffer cmd,const Vulkan::Instance& instance,const Pass& pass,
                      Vulkan::DynamicState& dynamic,vk::Pipeline& last_pipeline) {
    return RecordPassImpl(cmd,instance,pass,&dynamic,&last_pipeline);
}

#if !defined(NDEBUG)
RecordingCostProbes TakeRecordingCostProbes() noexcept {
    std::lock_guard lock(recording_probe_mutex);
    auto result=recording_probes; recording_probes={}; return result;
}
void ResetRecordingCostProbes() noexcept {
    std::lock_guard lock(recording_probe_mutex); recording_probes={};
}
#endif
}
