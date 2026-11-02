#pragma once
#include <memory>
#include <vector>
#include <boost/container/static_vector.hpp>
#include "shader_recompiler/resource.h"
#include "shader_recompiler/backend/bindings.h"
#include "ryty_gpu/native/descriptor_snapshot.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Native::Parallel {
// Immutable worker packet. The producer proves resource provenance and ordered
// submission-window borrowing; the epoch marker itself does not own GPU handles.
// Production extended admission currently excludes attachment clears.
struct Draw {
    vk::Pipeline pipeline{};
    vk::PipelineLayout layout{};
    std::shared_ptr<const void> pipeline_owner;
    Vulkan::DynamicState dynamic;
    Shader::PushData push{};
    std::array<vk::DescriptorSet,Shader::Backend::MaxDescriptorSets> sets{};
    u32 push_set=Shader::Backend::MaxDescriptorSets;
    u32 required_sets_mask{};
    bool immutable_upload_buffers_verified{};
    std::shared_ptr<const DescriptorSnapshot> push_writes;
    std::size_t descriptor_bytes{};
    // Complete, bounded vertex state owned by each packet. Handles are leased
    // by the same submission window as the pass; producer proves their origin.
    boost::container::static_vector<vk::VertexInputAttributeDescription2EXT,32> vertex_attributes{};
    boost::container::static_vector<vk::VertexInputBindingDescription2EXT,32> vertex_bindings{};
    boost::container::static_vector<vk::Buffer,32> vertex_buffers{};
    boost::container::static_vector<vk::DeviceSize,32> vertex_offsets{},vertex_sizes{},vertex_strides{};
    u32 vertex_attribute_count{},vertex_binding_count{},vertex_buffer_count{};
    bool vertex_backing_verified{};
    u32 vertices{}, instances{}, first_vertex{}, first_instance{};
    bool indexed{}, index_backing_verified{};
    vk::Buffer index_buffer{};vk::DeviceSize index_offset{},index_size{};
    vk::IndexType index_type{};s32 vertex_offset{};
};
struct Pass {
    Vulkan::RenderState target{};
    bool extended_verified{};
    // Submission-window marker, not an owning attachment reference. The producer
    // must verify tracked native views and their retirement through signal N.
    std::shared_ptr<const void> resource_lease;
    std::vector<Draw> draws;
};
// Reconstruct the worker-local dirty state from complete per-draw values;
// producer dirty bits belong to another command buffer and are not reusable.
inline void ApplyDynamicSnapshot(Vulkan::DynamicState& d,const Vulkan::DynamicState& s) {
    d.SetViewports(s.viewports);d.SetScissors(s.scissors);
    d.SetDepthTestEnabled(s.depth_test_enabled);d.SetDepthWriteEnabled(s.depth_write_enabled);d.SetDepthCompareOp(s.depth_compare_op);
    d.SetDepthBoundsTestEnabled(s.depth_bounds_test_enabled);d.SetDepthBounds(s.depth_bounds_min,s.depth_bounds_max);
    d.SetDepthBiasEnabled(s.depth_bias_enabled);d.SetDepthBias(s.depth_bias_constant,s.depth_bias_clamp,s.depth_bias_slope);
    d.SetStencilTestEnabled(s.stencil_test_enabled);d.SetStencilOps(s.stencil_front_ops,s.stencil_back_ops);
    d.SetStencilReferences(s.stencil_front_reference,s.stencil_back_reference);
    d.SetStencilWriteMasks(s.stencil_front_write_mask,s.stencil_back_write_mask);
    d.SetStencilCompareMasks(s.stencil_front_compare_mask,s.stencil_back_compare_mask);
    d.SetPrimitiveRestartEnabled(s.primitive_restart_enable);d.SetRasterizerDiscardEnabled(s.rasterizer_discard_enable);
    d.SetCullMode(s.cull_mode);d.SetFrontFace(s.front_face);d.SetBlendConstants(s.blend_constants);
    d.SetColorWriteMasks(s.color_write_masks);d.SetLineWidth(s.line_width);
    d.SetAttachmentFeedbackLoopEnabled(s.feedback_loop_enabled);
}
// Exact pass traversal, shared by the Vulkan wrapper and CPU sequence tests.
// Validation must finish before calling this: after begin there is no rollback.
template<class Begin, class Emit, class End>
void EmitWholePass(const Pass& pass,Begin&& begin,Emit&& emit,End&& end) {
    begin(pass.target);
    for(const auto& draw:pass.draws) emit(draw);
    end();
}
// Records one complete rendering scope into an already-begun secondary. The
// coordinator executes secondaries in original order and inserts original barriers.
// This routine neither resolves registry/guest state nor schedules GPU work.
bool ValidatePass(const Pass& pass);
bool RecordPass(vk::CommandBuffer cmd,const Vulkan::Instance& instance,const Pass& pass);
}
