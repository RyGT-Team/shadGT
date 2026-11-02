#include "ryty_gpu/native/parallel_pass_recording.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_command_stream.h"
#include <bit>
namespace Native::Parallel {
bool ValidatePass(const Pass& pass) {
    const auto& target=pass.target;
    if(target.num_color_attachments>8 || !target.width || !target.height || !target.num_layers ||
       !pass.resource_lease || pass.draws.empty() || pass.draws.size()>256) return false;
    if(!pass.extended_verified && (target.num_color_attachments!=1 || !target.color_attachments[0].image_view ||
       target.color_attachments[0].is_clear || target.depth_stencil_attachment.has_depth || target.depth_stencil_attachment.has_stencil))return false;
    if(pass.extended_verified && (target.depth_stencil_attachment.has_depth || target.depth_stencil_attachment.has_stencil) && !target.depth_stencil_attachment.image_view)return false;
    for(const auto& draw:pass.draws) {
        if(!draw.pipeline || !draw.layout || !draw.pipeline_owner || draw.push_set>draw.sets.size())return false;
        if(draw.indexed) {
            if(!pass.extended_verified || !draw.index_backing_verified || !draw.index_buffer ||
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
        for(u32 i=0;i<draw.vertex_buffer_count;++i)
            if(draw.vertex_bindings[i].binding!=i || draw.vertex_attributes[i].binding!=i)return false;
        for(u32 i=0;i<draw.vertex_buffer_count;++i)if(!draw.vertex_buffers[i] || !draw.vertex_sizes[i])return false;
        for(u32 i=0;i<draw.vertex_attribute_count;++i)if(draw.vertex_attributes[i].pNext)return false;
        for(u32 i=0;i<draw.vertex_binding_count;++i)if(draw.vertex_bindings[i].pNext)return false;
        static_assert(Shader::Backend::MaxDescriptorSets<=32);
        for(u32 i=0;i<draw.sets.size();++i) if((draw.required_sets_mask & (u32{1}<<i)) && !draw.sets[i])return false;
        if(draw.push_set<draw.sets.size() && (draw.required_sets_mask & (u32{1}<<draw.push_set)))return false;
        if(draw.push_writes) {
            if(draw.push_set>=draw.sets.size() || draw.sets[draw.push_set])return false;
            for(const auto& w:draw.push_writes->Writes()) {
                if(w.pNext || w.pTexelBufferView)return false;
                const bool image=w.descriptorType==vk::DescriptorType::eSampler || w.descriptorType==vk::DescriptorType::eSampledImage;
                const bool buffer=w.descriptorType==vk::DescriptorType::eUniformBuffer || w.descriptorType==vk::DescriptorType::eStorageBuffer;
                if(image ? (w.pBufferInfo || !w.pImageInfo) :
                   buffer ? (!draw.immutable_upload_buffers_verified || !w.pBufferInfo || w.pImageInfo) : true)return false;
            }
        }
    }
    return true;
}
bool RecordPass(vk::CommandBuffer cmd,const Vulkan::Instance& instance,const Pass& pass) {
    if(!cmd || !ValidatePass(pass)) return false;
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
    Vulkan::DynamicState dynamic{}; dynamic.Invalidate();
    vk::Pipeline bound_pipeline{};
    EmitWholePass(pass,
        [&](const auto&) {cmd.beginRendering(vk::RenderingInfo{.renderArea{.offset={0,0},.extent={target.width,target.height}},
            .layerCount=target.num_layers,.colorAttachmentCount=target.num_color_attachments,.pColorAttachments=colors.data(),
            .pDepthAttachment=db.has_depth?&depth:nullptr,.pStencilAttachment=db.has_stencil?&stencil:nullptr});},
        [&](const Draw& draw) {
            ApplyDynamicSnapshot(dynamic,draw.dynamic);dynamic.Commit(instance,commands);
            if(instance.IsVertexInputDynamicState()) {
                commands.setVertexInputEXT(vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT>{draw.vertex_binding_count,draw.vertex_bindings.data()},
                    vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT>{draw.vertex_attribute_count,draw.vertex_attributes.data()});
                if(draw.vertex_buffer_count) commands.bindVertexBuffers(0,draw.vertex_buffer_count,draw.vertex_buffers.data(),draw.vertex_offsets.data());
            } else if(draw.vertex_buffer_count) {
                commands.bindVertexBuffers2(0,draw.vertex_buffer_count,draw.vertex_buffers.data(),draw.vertex_offsets.data(),draw.vertex_sizes.data(),draw.vertex_strides.data());
            }
            if(bound_pipeline!=draw.pipeline) {commands.bindPipeline(vk::PipelineBindPoint::eGraphics,draw.pipeline);bound_pipeline=draw.pipeline;}
            commands.pushConstants(draw.layout,flags,0u,sizeof(draw.push),&draw.push);
            std::array<vk::DescriptorSet,Shader::Backend::MaxDescriptorSets> run{};u32 first=0,count=0;
            const auto flush=[&] {if(count){commands.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,draw.layout,first,
                vk::ArrayProxy<const vk::DescriptorSet>{count,run.data()},{});count=0;}};
            for(u32 i=0;i<draw.sets.size();++i) {
                if(i==draw.push_set && draw.push_writes && !draw.push_writes->Writes().empty()) {
                    flush();const auto writes=draw.push_writes->Writes();
                    commands.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics,draw.layout,i,
                        vk::ArrayProxy<const vk::WriteDescriptorSet>{static_cast<u32>(writes.size()),writes.data()});
                } else if(draw.sets[i]) {if(count && i!=first+count)flush();if(!count)first=i;run[count++]=draw.sets[i];}
            }
            flush();
            if(draw.indexed) {commands.bindIndexBuffer(draw.index_buffer,draw.index_offset,draw.index_type);
                commands.drawIndexed(draw.vertices,draw.instances,0,draw.vertex_offset,draw.first_instance);}
            else commands.draw(draw.vertices,draw.instances,draw.first_vertex,draw.first_instance);
        },
        [&] {cmd.endRendering();});
    return true;
}
}
