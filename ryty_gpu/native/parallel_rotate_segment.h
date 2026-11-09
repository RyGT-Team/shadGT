#pragma once
#include "ryty_gpu/native/parallel_pass_coordinator.h"
namespace Native::Parallel {
struct RotationDiag {u64 attempts{},success{},invalid{},preflight{},acquire{},command{},prefix{},exception{};};
// Rotate without NativeRenderer::Begin/GlobalBarrier recursion. Caller must
// preserve all cross-segment hazards, uploads and pool ownership, and must
// pass a real submission-window resource lease. This helper is not connected
// to the live renderer until its failure/retirement contract is proven.
template<class ResetState>
bool RotateSegment(PassCoordinator& ordered,Rhi::Device& device,u64 epoch,
                   Rhi::CommandList*& list,vk::CommandBuffer& cmd,
                   std::shared_ptr<const void> lease,std::size_t charged_bytes,
                   ResetState&& reset_state,RotationDiag* diag=nullptr) {
#if !defined(NDEBUG)
    PassCoordinator::TimingProbe timing{ordered,PassCoordinator::TimingPhase::Rotation};
#endif
    if(diag)++diag->attempts;
    if(!list || !cmd || !lease){if(diag)++diag->invalid;return false;}
    Rhi::CommandList* replacement=nullptr;bool queued=false;
    try {
        // A full budget may become available immediately afterwards; in that
        // case this draw conservatively follows the existing serial fallback.
        if(!ordered.PrefixPreflight(epoch,list,lease,charged_bytes)){if(diag)++diag->preflight;return false;}
        replacement=device.AcquireCommandList(Rhi::CommandListLevel::Secondary);
        if(!replacement){if(diag)++diag->acquire;return false;}
        replacement->Begin();
        const auto next=vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(replacement->NativeHandle())};
        if(!next) {if(diag)++diag->command;device.ReleaseUnsubmitted(replacement);return false;}
        if(!ordered.Prefix(epoch,list,std::move(lease),charged_bytes)) {
            if(diag)++diag->prefix;
            device.ReleaseUnsubmitted(replacement);return false;
        }
        // RHI End and cache invalidation must not throw after the prefix has
        // entered the ordered queue; callers must treat violation as fatal.
        queued=true;
        list->End();list=replacement;cmd=next;reset_state();if(diag)++diag->success;
#if !defined(NDEBUG)
        timing.Success();
#endif
        return true;
    } catch(...) {
        if(queued) std::terminate(); // no safe rollback after queue publication
        if(diag)++diag->exception;
        if(replacement)device.ReleaseUnsubmitted(replacement);
        return false;
    }
}
}
