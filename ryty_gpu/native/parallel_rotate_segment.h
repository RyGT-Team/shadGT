#pragma once
#include "ryty_gpu/native/parallel_pass_coordinator.h"
namespace Native::Parallel {
// Rotate without NativeRenderer::Begin/GlobalBarrier recursion. Caller must
// preserve all cross-segment hazards, uploads and pool ownership, and must
// pass a real submission-window resource lease. This helper is not connected
// to the live renderer until its failure/retirement contract is proven.
template<class ResetState>
bool RotateSegment(PassCoordinator& ordered,Rhi::Device& device,u64 epoch,
                   Rhi::CommandList*& list,vk::CommandBuffer& cmd,
                   std::shared_ptr<const void> lease,std::size_t charged_bytes,
                   ResetState&& reset_state) {
    if(!list || !cmd || !lease)return false;
    Rhi::CommandList* replacement=nullptr;bool queued=false;
    try {
        replacement=device.AcquireCommandList(Rhi::CommandListLevel::Secondary);
        if(!replacement)return false;
        replacement->Begin();
        const auto next=vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(replacement->NativeHandle())};
        if(!next || !ordered.Prefix(epoch,list,std::move(lease),charged_bytes)) {
            device.ReleaseUnsubmitted(replacement);return false;
        }
        // RHI End and cache invalidation must not throw after the prefix has
        // entered the ordered queue; callers must treat violation as fatal.
        queued=true;
        list->End();list=replacement;cmd=next;reset_state();return true;
    } catch(...) {
        if(queued) std::terminate(); // no safe rollback after queue publication
        if(replacement)device.ReleaseUnsubmitted(replacement);
        return false;
    }
}
}
