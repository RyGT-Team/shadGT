#include "ryty_gpu/native/ahead_descriptor_pools.h"
#include "ryty_gpu/native/per_draw_view_proof.h"
#include "ryty_gpu/native/adaptive_xxh3.h"
#include "ryty_gpu/native/batch_view_walk.h"
#include "parallel_absent_sampled_image.h"
#include "ryty_gpu/native/parallel_mode_control.h"
#include "ryty_gpu/native/sync_owner_pins.h"
#include "ryty_gpu/native/rhi/completed_owner_release.h"
#include "ryty_gpu/native/prepared_direct_slot.h"
#include "ryty_gpu/native/readonly_hint.h"
#include "ryty_gpu/native/parallel_direct_capability.h"
#include "ryty_gpu/native/parallel_gate_histogram.h"
#include "core/emulator_settings.h"
#include "ryty_gpu/native/parallel_rotate_segment.h"
#include "ryty_gpu/native/natural_scope_boundary.h"
#include "ryty_gpu/native/attachment_alias.h"
#include "ryty_gpu/native/prepared_index_bounds.h"
#include "ryty_gpu/native/prepared_direct_absent.h"
#include "ryty_gpu/native/per_draw_stage_proof.h"
#include "ryty_gpu/native/positive_handle_cache.h"
#include "parallel_submit_context.h"
#include "ryty_gpu/native/descriptor_alloc_metrics.h"
#include "a82_capture_gate.h"
#include "ryty_gpu/native/descriptor_pool_reuse.h"
#include "ryty_gpu/native/epoch_lookup_memo.h"
#include "ryty_gpu/native/descriptor_update_batch.h"
#include "ryty_gpu/native/upload_phase_trace.h"
#include "ryty_gpu/native/region_init_gaps.h"
#include "ryty_gpu/native/upload_cause.h"
#include "ryty_gpu/native/local_tiled_source.h"
#include "video_core/renderer_vulkan/async_mirror_buffer.h"
#include "video_core/renderer_vulkan/descriptor_write_groups.h"
#include "video_core/renderer_vulkan/upload_bytes_equal.h"
#include "video_core/renderer_vulkan/immutable_upload_memo.h"
#include "video_core/renderer_vulkan/freshness_range_search.h"
#include "video_core/renderer_vulkan/draw_preparation_options.h"
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// NAT-100: native mode (RYGT_NATIVE=1, ryGT builds). Draws, dispatches and copies are recorded
// with the native image registry and guest memory read in place, instead of the texture and
// buffer caches, which keep images and buffers in step with guest memory draw by draw and are
// most of the recorder's cost. Version 0 records on the recorder thread, in GPU order, into a
// secondary command buffer; before anything else goes into the scheduler's command buffer (a
// submit, a flip) that list is ended and run there, so fences, labels and presenting work as
// before. Images stay in the general layout. Synchronisation is by global memory barriers: when
// a pass starts with other targets, before a draw reads a picture written since the last
// barrier, and around dispatches and copies. Later versions record the game's render jobs on its
// own threads with this code (ryty_gpu/native/DESIGN.md, N4).

#include "video_core/renderer_vulkan/vk_rasterizer_native.h"
#include "video_core/renderer_vulkan/scene_region_scale.h"
#include "video_core/renderer_vulkan/upload_chunk_tail.h"
#include "video_core/renderer_vulkan/upload_chunk_cache.h"

#include <fstream>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

#include <fmt/ranges.h>
#include <magic_enum/magic_enum.hpp>
#include <xxhash.h>

#include "ryty_gpu/ryty_upscaler_status.h"
#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/perf_monitor.h"
#include "common/ryty_config.h"
// (ryty_config.h brings in windows.h, whose `near` and `far` macros would rename variables here.)
#undef near
#undef far
#include "core/guest_hash.h"
#include "core/memory.h"
#include "ryty_gpu/native/image_registry.h"
#include "ryty_gpu/native/cpu_texture_staging.h"
#include "ryty_gpu/native/gpu_checkpoints.h"
#include "ryty_gpu/native/slow_call_trace.h"
#include "ryty_gpu/native/job_walker.h"
#include "ryty_gpu/native/rhi/rhi_vulkan.h"
#include "ryty_gpu/ryty_ghost.h"
#include "ryty_gpu/ryty_fsr3.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/direct_memory.h"
#include "video_core/renderer_vulkan/depth_attachment.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/resource_binding.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_native_ui.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image_descriptor.h"
#include "video_core/texture_cache/sampler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace VideoCore {
vk::ImageViewType ConvertImageViewType(AmdGpu::ImageType type);
}

#include "video_core/renderer_vulkan/scene_capture_trace.h"
#include "video_core/renderer_vulkan/trace_mip_layout.h"
#include "video_core/renderer_vulkan/trace_residency.h"
#include "video_core/renderer_vulkan/resident_prt_view.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace Vulkan {
namespace {
struct AheadVulkanBackend {
    using Pool=vk::DescriptorPool;using Layout=vk::DescriptorSetLayout;using Set=vk::DescriptorSet;
    static constexpr std::size_t MaxSetsPerPool=256;
    static constexpr std::array<std::size_t,4> MaxDescriptorsPerPool{4096,4096,512,2048};
    vk::Device device;
    Pool Create() noexcept {try {
        const std::array<vk::DescriptorPoolSize,4> sizes{{{vk::DescriptorType::eStorageBuffer,4096},{vk::DescriptorType::eSampledImage,4096},{vk::DescriptorType::eStorageImage,512},{vk::DescriptorType::eSampler,2048}}};
        const auto [result,pool]=device.createDescriptorPool(vk::DescriptorPoolCreateInfo{.maxSets=u32(MaxSetsPerPool),.poolSizeCount=u32(sizes.size()),.pPoolSizes=sizes.data()});
        return result==vk::Result::eSuccess?pool:Pool{};
    } catch(...) {return {};}}
    bool Reset(Pool pool) noexcept {try{return device.resetDescriptorPool(pool)==vk::Result::eSuccess;}catch(...){return false;}}
    bool Allocate(Pool pool,std::span<const Layout> layouts,std::span<Set> sets) noexcept {try {
        const vk::DescriptorSetAllocateInfo a{.descriptorPool=pool,.descriptorSetCount=u32(layouts.size()),.pSetLayouts=layouts.data()};
        return device.allocateDescriptorSets(&a,sets.data())==vk::Result::eSuccess;
    } catch(...) {return false;}}
    void Destroy(Pool pool) noexcept {device.destroyDescriptorPool(pool);}
};
using AheadPools=Native::Parallel::AheadDescriptorPools<AheadVulkanBackend,4,Shader::Backend::MaxDescriptorSets>;
struct AheadContext {
    AheadPools pools;std::atomic<bool> closing{},mode{};std::atomic<u64> active{},attempts{},ready{},used{},unused{},used_sets{},unused_sets{},failures{},capacity{};
    explicit AheadContext(vk::Device device):pools(AheadVulkanBackend{device}){}
};
struct AheadLease {
    std::shared_ptr<AheadContext> context;AheadPools::LeasePtr lease;
    std::array<vk::DescriptorSet,Shader::Backend::MaxDescriptorSets> sets{};
    std::array<vk::DescriptorSetLayout,Shader::Backend::MaxDescriptorSets> layouts{};
    vk::PipelineLayout pipeline_layout{};std::shared_ptr<const void> pipeline_owner;std::size_t prepared_sets{},used_sets{};bool used{};
    ~AheadLease(){if(context && lease){if(!used)context->unused.fetch_add(1,std::memory_order_relaxed);context->unused_sets.fetch_add(prepared_sets-used_sets,std::memory_order_relaxed);}}
};
std::atomic<u32> ahead_enabled_contexts{};
void SetAheadMode(AheadContext& c,bool enabled) noexcept {
    const bool old=c.mode.exchange(enabled,std::memory_order_acq_rel);if(old==enabled)return;
    c.pools.SetEnabled(enabled);if(enabled)ahead_enabled_contexts.fetch_add(1,std::memory_order_release);else ahead_enabled_contexts.fetch_sub(1,std::memory_order_release);
}
std::mutex ahead_context_mutex;
std::unordered_map<const NativeRenderer*,std::shared_ptr<AheadContext>> ahead_contexts;
bool AheadRequested() noexcept {static const bool enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_PRODUCER_DESCRIPTORS");return !p || (p[0]=='1' && !p[1]);}();return enabled;}

std::shared_ptr<AheadContext> FindAheadBlocking(const NativeRenderer* renderer) {
    if(!AheadRequested())return {};std::scoped_lock lock{ahead_context_mutex};const auto it=ahead_contexts.find(renderer);
    return it==ahead_contexts.end()?std::shared_ptr<AheadContext>{}:it->second;
}
std::shared_ptr<AheadContext> FindAhead(const NativeRenderer* renderer) noexcept {
    if(!ahead_enabled_contexts.load(std::memory_order_acquire))return {};
    std::unique_lock lock{ahead_context_mutex,std::try_to_lock};if(!lock)return {};
    const auto it=ahead_contexts.find(renderer);if(it==ahead_contexts.end())return {};
    auto c=it->second;if(!c->mode.load(std::memory_order_acquire) || c->closing.load(std::memory_order_acquire))return {};
    c->active.fetch_add(1,std::memory_order_acq_rel);return c;
}
std::shared_ptr<AheadContext> BeginAheadShutdown(const NativeRenderer* renderer) {
    if(!AheadRequested())return {};std::scoped_lock lock{ahead_context_mutex};const auto it=ahead_contexts.find(renderer);
    if(it==ahead_contexts.end())return {};auto c=it->second;c->closing.store(true,std::memory_order_release);SetAheadMode(*c,false);c->pools.BeginClose();ahead_contexts.erase(it);return c;
}
struct AheadPrepareGuard {
    std::shared_ptr<AheadContext> context;
    explicit AheadPrepareGuard(std::shared_ptr<AheadContext> c):context(std::move(c)) {}
    ~AheadPrepareGuard(){if(context)context->active.fetch_sub(1,std::memory_order_release);}
};
}
namespace {
// Experimental sidecar: enabled only by RYGT_PARALLEL_RECORDING=1. The
// renderer's recursive mutex serializes access to each sidecar; workers only
// consume immutable pass packets and RHI device/instance handles.
struct ParallelBindingCapture {
    std::array<vk::DescriptorSet,Shader::Backend::MaxDescriptorSets> sets{};
    AheadLease* ahead{};bool ahead_used{},write_structure_proven{},counts_proven{};
    std::size_t ordinary_writes{},ordinary_buffers{},ordinary_images{};
};
thread_local ParallelBindingCapture* parallel_binding_capture=nullptr;
struct ScopedParallelBindingCapture {
    ParallelBindingCapture* previous;
    explicit ScopedParallelBindingCapture(ParallelBindingCapture& capture):previous(parallel_binding_capture) {parallel_binding_capture=&capture;}
    ~ScopedParallelBindingCapture(){parallel_binding_capture=previous;}
};
struct ProofPhaseStats {u64 count{},wall_ns{},wall_max{};};
struct RecordPartitionMetric {u64 count{},wall_ns{};};
bool BatchViewWalkRequested() noexcept {
    static const bool enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_BATCH_VIEW_WALK");return p && p[0]=='1' && !p[1];}();
    return enabled;
}
bool TransferPassesRequested() noexcept {
    static const bool enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_TRANSFER_PASSES");return p && p[0]=='1' && !p[1];}();
    return enabled;
}
bool PreparedSampledProofRequested() noexcept {
    static const bool enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_PREPARED_VIEW_PROOF");return p && p[0]=='1' && !p[1];}();
    return enabled;
}
Native::Parallel::ProofProducerRegistry prepared_proof_contexts;
// Recorder-owned host update batch. Storage never moves while any write points into it.
struct PendingDescriptorUpdates {
    PendingDescriptorUpdates()=default;
    PendingDescriptorUpdates(const PendingDescriptorUpdates&)=delete;
    PendingDescriptorUpdates& operator=(const PendingDescriptorUpdates&)=delete;
    PendingDescriptorUpdates(PendingDescriptorUpdates&&)=delete;
    PendingDescriptorUpdates& operator=(PendingDescriptorUpdates&&)=delete;
    static_assert(256*sizeof(vk::WriteDescriptorSet)+1024*sizeof(vk::DescriptorBufferInfo)+
                  1024*sizeof(vk::DescriptorImageInfo)<=64*1024);
    enum Reason : u32 { EndPass, Serial, Abandoned, Capacity, Unsupported, Exception, ReasonCount };
    enum class Admission { Yes, Capacity, Unsupported };
    std::array<vk::WriteDescriptorSet,256> writes{};
    std::array<vk::DescriptorBufferInfo,1024> buffers{};
    std::array<vk::DescriptorImageInfo,1024> images{};
    std::size_t nw{},nb{},ni{};
    u64 logical_draws{},actual_calls{},staged_rows{},immediate_draws{},overflow_draws{},unsupported_draws{},high_rows{};
    std::array<u64,ReasonCount> flushes{};
    template<bool Proven=false,class Groups>
    Admission Check(const Groups& groups,u32 push,std::span<const vk::DescriptorBufferInfo> source_buffers,
                    std::span<const vk::DescriptorImageInfo> source_images) const noexcept {
        std::size_t w=nw,b=nb,i=ni;
        for(u32 set=0;set<groups.size();++set) if(set!=push) for(const auto& row:groups[set]) {
            if constexpr(Proven) {
                if(row.pBufferInfo) {if(row.descriptorCount>buffers.size()-b)return Admission::Capacity;b+=row.descriptorCount;}
                else {if(row.descriptorCount>images.size()-i)return Admission::Capacity;i+=row.descriptorCount;}
            } else {
                if(row.pNext || row.pTexelBufferView || !row.descriptorCount)return Admission::Unsupported;
                const bool buffer=row.descriptorType==vk::DescriptorType::eUniformBuffer || row.descriptorType==vk::DescriptorType::eStorageBuffer;
                const bool image=row.descriptorType==vk::DescriptorType::eSampler || row.descriptorType==vk::DescriptorType::eSampledImage;
                if(buffer) {if(row.pImageInfo || !DescriptorSnapshot::BufferSliceValid(row.pBufferInfo,row.descriptorCount,source_buffers))return Admission::Unsupported;
                    if(row.descriptorCount>buffers.size()-b)return Admission::Capacity;b+=row.descriptorCount;}
                else if(image) {if(row.pBufferInfo || !DescriptorSnapshot::ImageSliceValid(row.pImageInfo,row.descriptorCount,source_images))return Admission::Unsupported;
                    if(row.descriptorCount>images.size()-i)return Admission::Capacity;i+=row.descriptorCount;}
                else return Admission::Unsupported;
            }
            if(w==writes.size())return Admission::Capacity;++w;
        }
        return Admission::Yes;
    }
    Admission CheckCounts(std::size_t w,std::size_t b,std::size_t i) const noexcept {
        return Native::DescriptorCountsFit(w,b,i,writes.size()-nw,buffers.size()-nb,images.size()-ni) ? Admission::Yes : Admission::Capacity;
    }
    template<class Rows> void Stage(const Rows& rows,vk::DescriptorSet set) noexcept {
        for(const auto& source:rows) {
            auto row=source;row.dstSet=set;
            if(row.pBufferInfo) {std::copy_n(row.pBufferInfo,row.descriptorCount,buffers.data()+nb);row.pBufferInfo=buffers.data()+nb;nb+=row.descriptorCount;}
            else {std::copy_n(row.pImageInfo,row.descriptorCount,images.data()+ni);row.pImageInfo=images.data()+ni;ni+=row.descriptorCount;}
            writes[nw++]=row;++staged_rows;
        }
        high_rows=std::max<u64>(high_rows,nw);
    }
    void Flush(vk::Device device,Reason reason) noexcept {
        if(!nw)return;
        device.updateDescriptorSets(vk::ArrayProxy<const vk::WriteDescriptorSet>{static_cast<u32>(nw),writes.data()},{});
        ++actual_calls;++flushes[reason];nw=nb=ni=0;
    }
};
struct ParallelRuntime {
    // Mode at Draw's record-phase entry; every metric uses the existing 1/16 sample.
    std::array<std::array<RecordPartitionMetric,5>,2> record_partition{};
    Native::Parallel::RotationDiag rotation_diag{};
    std::array<u64,9> pass_size_hist{};
    u64 small_admission_exceptions{},large_admission_exceptions{};
    u64 serial_split_attempts{},serial_split_success{},serial_split_first_draws{};
    PendingDescriptorUpdates descriptor_updates;
    u64 producer_structure_hits{},producer_vertex_hits{},producer_snapshot_hits{};
    u64 producer_action_hits{},producer_upload_lookup_hits{},producer_stage_schema_hits{},producer_stage_lookup_hits{},clip_fast_hits{},direct_table_cache_hits{};
    std::unique_ptr<Native::Parallel::PassCoordinator> coordinator;
    std::unique_ptr<Native::Parallel::ModeController> mode_controller;
    Native::Parallel::AppliedMode applied_mode{true};bool mode_initial_logged{};
    std::shared_ptr<Native::Parallel::ProofProducerContext> prepared_proof_context;
    std::unique_ptr<Native::Parallel::ModeController> transfer_mode_controller;
    Native::Parallel::AppliedMode transfer_applied_mode{TransferPassesRequested()};bool transfer_mode_initial_logged{};
    std::unique_ptr<Native::Parallel::ModeController> view_mode_controller;
    Native::Parallel::AppliedMode view_applied_mode{BatchViewWalkRequested()};bool view_mode_initial_logged{};
    u64 whole_seq{},whole_samples{},whole_wall_ns{},whole_cpu_samples{},whole_cpu_100ns{},whole_max_ns{};
    ParallelGateHistogram gates;
    std::optional<Native::Parallel::Pass> active;
    Native::Parallel::SyncOwnerPins<> sync_pins;
    std::size_t pin_slot=Native::Parallel::SyncOwnerPins<>::Invalid;
    std::vector<Native::Parallel::Draw> scratch_draws;
    std::shared_ptr<const void> lease;
    u64 epoch{};
    std::array<u64,10> descriptor_fail_reasons{};u32 descriptor_samples{};u64 descriptor_readonly_attempts{},descriptor_readonly_regions{},descriptor_readonly_imports{},descriptor_readonly_uncontained{},descriptor_readonly_unmatched{},descriptor_clip_nulls{},readonly_range_hint_hits{},readonly_range_hint_misses{},readonly_region_hint_hits{},readonly_region_hint_misses{};
    std::array<ProofPhaseStats,9> proof_phases{};
    Native::Parallel::PositiveHandleCache<vk::Sampler> sampler_proof;
    u64 sampler_proof_hits{},sampler_proof_misses{},sampler_scan_steps{},sampler_scan_positives{};
    u64 natural_preclosed{},natural_captured{},natural_serial_fallback{},natural_binding_skip{};
    u64 absent_mode0_attempts{},absent_mode0_accepted{},absent_mode0_rejected{};
    u64 sampled_null_view{},sampled_nonnull_sampler{},sampled_nongeneral{},sampled_canonical_null{};
    u64 absent_sampled_attempts{},absent_sampled_accepted{},absent_sampled_rejected{};
    u64 prepared_view_attempts{},prepared_view_accepted{},prepared_view_fallbacks{};
    std::array<u64,7> captured_vertex_counts{};u64 captured_vertex_sum{};
    Native::Parallel::ViewProofStats view_proof_stats{};
    struct ViewSample {u64 sequence{},draws{},queried_draws{},image_infos{},sampled_entries{};std::array<u64,10> cardinality{};Native::Parallel::ViewProofStats proof{};} view_sample;
    struct AheadFlight {std::shared_ptr<AheadLease> lease;std::function<void()> release;u64 tick{};};
    std::array<AheadFlight,2048> ahead_flights{};std::size_t ahead_cursor{},ahead_live{};std::shared_ptr<AheadContext> ahead_context;
    u64 proof_table_builds{},proof_table_hits{},extended_clear_rejected{},fast_rejected{},no_pass_room_rejected{},serial_scope_bypassed{},owner_slot_pressure{},eligible_in_serial_scope{},eligible_in_active_scope{},eligible_at_boundary{},vertex_candidates{},vertex_preflight_draws{},vertex_backing_pass{},vertex_bad_structure{},vertex_bad_binding{},vertex_bad_pnext{},vertex_unknown_backing{},vertex_direct_ranges{},vertex_direct_attempts{},vertex_direct_matches{},direct_slot_attempts{},direct_slot_accepted{},passes{},draws{},queued{},synchronous{},considered{},eligible{},unknown_resource_key{},buffer_rejected{},alias_rejected{},descriptor_rejected{},missing_sets{},preflight_seq{},preflight_samples{},preflight_wall_ns{},preflight_cpu_100ns{},preflight_cpu_samples{},drain_ns{},coordinator_cpu_100ns{},coordinator_cpu_samples{};
    u64 serial_eligible_run{},serial_eligible_run_max{};std::array<u64,4> serial_eligible_runs{};
    void FinishSerialEligibleRun() noexcept {
        if(!serial_eligible_run)return;
        const auto n=serial_eligible_run;++serial_eligible_runs[n==1?0:n<4?1:n<8?2:3];serial_eligible_run=0;
    }
    void NoteSerialEligible() noexcept {
        if(serial_eligible_run!=UINT64_MAX)++serial_eligible_run;
        serial_eligible_run_max=std::max(serial_eligible_run_max,serial_eligible_run);
    }
    bool closing{};
};
std::mutex parallel_sidecars_mutex;
std::unordered_map<const NativeRenderer*,std::unique_ptr<ParallelRuntime>> parallel_sidecars;
std::atomic<u64> parallel_sidecars_generation{1};
std::atomic<bool> parallel_sidecar_init_failed{false};
bool ParallelRecordingRequested();
ParallelRuntime* FindParallelRuntime(const NativeRenderer* renderer) {
    if(!ParallelRecordingRequested())return nullptr;
    struct Cache {const NativeRenderer* owner{};ParallelRuntime* sidecar{};u64 generation{};};
    static thread_local Cache cache;
    const u64 generation=parallel_sidecars_generation.load(std::memory_order_acquire);
    if(cache.owner==renderer && cache.generation==generation)return cache.sidecar;
    std::lock_guard lock(parallel_sidecars_mutex);
    const auto it=parallel_sidecars.find(renderer);
    cache={renderer,it==parallel_sidecars.end()?nullptr:it->second.get(),parallel_sidecars_generation.load(std::memory_order_relaxed)};
    return cache.sidecar;
}
bool ParallelRecordingRequested() {
    static const bool requested=[] {const char* p=std::getenv("RYGT_PARALLEL_RECORDING");return p&&p[0]=='1'&&p[1]=='\0';}();return requested;
}
bool ParallelReadonlyDescriptorEnabled() {static const bool yes=[] {const char* p=std::getenv("RYGT_PARALLEL_READONLY_DESCRIPTOR");return p && p[0]=='1' && p[1]=='\0';}();return yes;}
bool ParallelDirectVertexEnabled() { static const bool yes=[] {const char* p=std::getenv("RYGT_PARALLEL_DIRECT_VERTEX");return p && p[0]=='1' && p[1]=='\0';}();return yes; }
bool ParallelVertexEnabled() { static const bool yes=[] {const char* p=std::getenv("RYGT_PARALLEL_VERTEX");return p && p[0]=='1' && p[1]=='\0';}();return yes; }
bool ParallelSmallPassBatchEnabled() { static const bool enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_SMALLPASS_BATCH");return p && p[0]=='1' && p[1]=='\0';}();return enabled; }
// Experimental indexed/MRT/depth packet. Clear admission remains separately gated.
bool ParallelExtendedEnabled() {static const bool on=[] {const char* p=std::getenv("RYGT_PARALLEL_EXTENDED");return p && p[0]=='1' && !p[1];}();return on;}
bool ParallelD2Enabled() { static const bool yes=[] {const char* p=std::getenv("RYGT_PARALLEL_D2"); return p && p[0]=='1' && p[1]=='\0';}();return yes; }
bool ParallelProofPhaseEnabled() {static const bool enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_PROOF_PHASE_DIAG");return p && p[0]=='1' && !p[1];}();return enabled;}
bool ParallelGateDiagEnabled() {
    static const bool enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_GATE_DIAG");return p&&p[0]=='1'&&p[1]=='\0';}();return enabled;
}
struct ParallelEligibilityStats {
    u64 observed{}, eligible{}, race{}, nonindexed{}, single_color{}, no_clear{}, no_depth{}, no_descriptors{}, no_storage{}, no_feedback{}, eligible_draws_in_runs[7]{}, runs[7]{}, current{};
    void Flush() { if (!current) return; unsigned bucket=0; u64 value=current; while(value>1 && bucket<6){value>>=1;++bucket;} ++runs[bucket]; eligible_draws_in_runs[bucket]+=current;current=0; }
};
thread_local ParallelEligibilityStats parallel_eligibility_stats;
thread_local const NativeRenderer* parallel_eligibility_owner=nullptr;
bool ParallelEligibilityEnabled() { static const bool enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_ELIGIBILITY_DIAG");return p&&p[0]=='1';}();return enabled; }
thread_local Native::DescriptorAllocMetrics descriptor_alloc_metrics;
thread_local u64 descriptor_bind_sets[2]{}, descriptor_bind_runs[2]{}, descriptor_bind_multi[2]{};
thread_local const NativeRenderer* descriptor_alloc_owner = nullptr;
bool DescriptorAllocDiagEnabled() {
    static const bool enabled = [] { const char* e=std::getenv("RYGT_DESC_ALLOC_DIAG"); return e && e[0]=='1'; }();
    return enabled;
}
bool DescriptorThreadCpu100ns(u64& result) {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        result=(u64(kernel.dwHighDateTime)<<32 | kernel.dwLowDateTime) + (u64(user.dwHighDateTime)<<32 | user.dwLowDateTime);
        return true;
    }
#endif
    result=0; return false;
}
struct SampledProofTimer {
    ProofPhaseStats* stats{};std::chrono::steady_clock::time_point start{};
    SampledProofTimer(ProofPhaseStats& s,bool sampled) noexcept {if(sampled){stats=&s;start=std::chrono::steady_clock::now();}}
    ~SampledProofTimer() {if(!stats)return;const auto raw=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();const u64 wall=raw>0?u64(raw):0;
        ++stats->count;stats->wall_ns+=wall;stats->wall_max=std::max(stats->wall_max,wall);}
};
template<class F> auto TimedProofInvoke(ProofPhaseStats& stats,bool sampled,F&& fn) {
    if(!sampled)return std::forward<F>(fn)();
    SampledProofTimer timer{stats,true};return std::forward<F>(fn)();
}
}
struct SceneIndirectArgs {u64 id,address,offset;};
static std::unique_ptr<Native::Rhi::Buffer> scene_indirect_buffer;
static std::vector<SceneIndirectArgs> scene_indirect_args;

bool RytyGtsInRace();
extern std::atomic<u64> g_prepared_frames;
bool RytyGtsMenuSnapshotsAllowed();
namespace {
bool A82CaptureAfterRace() {
    static const bool enabled=[] { const char* p=std::getenv("RYGT_A82_CAPTURE_AFTER_RACE"); return p && p[0]=='1'; }();
    if (!enabled) return true;
    static A82RaceGate gate;
    static std::atomic<bool> logged_race{}, logged_armed{};
    const auto state=gate.Observe(RytyGtsInRace());
    if (state==A82RaceGate::SawRace && !logged_race.exchange(true))
        LOG_WARNING(Render_Vulkan,"DIAG-A82 gate observed race; capture budget remains untouched");
    if (state==A82RaceGate::Armed && !logged_armed.exchange(true))
        LOG_WARNING(Render_Vulkan,"DIAG-A82 gate armed after race; capture enabled");
    return state==A82RaceGate::Armed;
}
thread_local u64 descriptor_hash_reused = 0;
thread_local u64 descriptor_hash_refreshed = 0;

using FlatUploadMemo = ImmutableUploadMemo<vk::DescriptorBufferInfo, Shader::MaxStageTypes>;
std::atomic<u64> sampler_cache_epoch{1};

FlatUploadMemo& UploadMemoFor(const NativeRenderer* renderer) {
    struct Local { const NativeRenderer* owner{}; FlatUploadMemo memo; };
    static thread_local Local local;
    if (local.owner != renderer) { local.memo.Clear(); local.owner = renderer; }
    return local.memo;
}


constexpr bool IsSceneCompositionShader(u64 hash) {
    // Race composition and the transmission-selection menu variant, captured at 540p.
    return hash == 0xb33ec4df || hash == 0xcc4e73c3;
}

// The experimental scene leaves GT Sport's final composition T# at output size.
// Resolve that one sampled view against a matching, already-rendered physical target.
// False means a preparing worker must defer to the ordered recorder, where earlier
// draws have registered their targets. No descriptor or registry entry is mutated.
bool MatchExperimentalCompositionTexture(AmdGpu::Image& sharp, const Shader::Info& stage,
                                         const Shader::ImageResource& resource,
                                         Native::ImageRegistry& registry) {
    static const bool enabled = [] {
        const char* env = std::getenv("RYGT_EXPERIMENTAL_SCENE_VIEWPORT");
        return env && std::strcmp(env, "1") == 0;
    }();
    static const u32 scene_width = [] {
        const char* value = std::getenv("RYGT_SCENE_WIDTH");
        return value ? u32(std::strtoul(value, nullptr, 10)) : 960u;
    }();
    static const u32 scene_height = [] {
        const char* value = std::getenv("RYGT_SCENE_HEIGHT");
        return value ? u32(std::strtoul(value, nullptr, 10)) : 540u;
    }();
    if (scene_width < 320 || scene_width > 3840 || scene_height < 180 || scene_height > 2160 ||
        (scene_width == 1920 && scene_height == 1080)) return true;
    if (enabled && IsSceneCompositionShader(stage.pgm_hash) && !resource.is_written && !resource.is_depth &&
        sharp.GetType() == AmdGpu::ImageType::Color2D && sharp.width + 1 == scene_width &&
        sharp.height + 1 == scene_height) {
        // Already-correct descriptors need no resolution fix, but still identify the
        // actual scene separately from the full-resolution composition attachment.
        const auto format = LiverpoolToVK::TrySurfaceFormat(sharp.GetDataFmt(), sharp.GetNumberFmt());
        const auto* observed = registry.FindExisting(sharp.Address(), [&](const Native::ImageEntry& entry) {
            return !entry.from_cpu && !entry.info.props.is_depth &&
                entry.info.pixel_format == format && entry.info.size.width == scene_width &&
                entry.info.size.height == scene_height && entry.info.pitch == sharp.Pitch();
        }, true);
        if (observed) RytyUpscalerStatus::ObserveScene(observed->info.size.width, observed->info.size.height);
    }
    const bool full_scene = sharp.width == 1919 && sharp.height == 1079;
    // This menu path builds sampled T# dimensions in 1080p logical coordinates.
    // Its scene setter already scales the viewport; repair only the sampled region,
    // and only after finding the known physical scene allocation below.
    const bool menu_region = stage.pgm_hash == 0xcc4e73c3 &&
        sharp.width < 1920 && sharp.height < 1080;
    if (!enabled || !IsSceneCompositionShader(stage.pgm_hash) || resource.is_written || resource.is_depth ||
        sharp.GetType() != AmdGpu::ImageType::Color2D || (!full_scene && !menu_region) ||
        sharp.NumLevels() != 1 ||
        sharp.NumLayers() != 1 || sharp.base_level != 0) {
        return true;
    }
    const auto format = LiverpoolToVK::TrySurfaceFormat(sharp.GetDataFmt(), sharp.GetNumberFmt());
    const auto tile_mode = sharp.GetTileMode();
    const auto* target = registry.FindExisting(sharp.Address(), [&](const Native::ImageEntry& entry) {
        const auto& info = entry.info;
        return !entry.from_cpu && !info.props.is_depth && info.pixel_format == format &&
               info.size.width == scene_width && info.size.height == scene_height && info.size.depth == 1 &&
               info.pitch == sharp.Pitch() && info.tile_mode == tile_mode && info.num_samples == 1 &&
               info.resources.levels == 1 && info.resources.layers == 1;
    }, true);
    // Auxiliary menu textures are not scene regions. Leave them untouched when
    // there is no matching physical scene allocation at their address.
    if (!target) return menu_region && !full_scene;
    const auto extent = menu_region
        ? ScaleSceneRegion(sharp.width + 1, sharp.height + 1, scene_width, scene_height)
        : SceneRegionExtent{scene_width, scene_height};
    const auto [width, height] = extent;
    if (!width || !height || width > target->info.size.width || height > target->info.size.height)
        return false;
    // Keep the real tiled row pitch. ImageRegistry::PlaceOf links this corner crop
    // to the rendered target, rather than uploading a second interpretation of RAM.
    sharp.width = width - 1;
    sharp.height = height - 1;
    RytyUpscalerStatus::ObserveScene(target->info.size.width, target->info.size.height);
    static std::mutex log_mutex;
    static std::unordered_set<u64> logged_regions;
    {
        std::scoped_lock lock{log_mutex};
        const u64 key = (u64(width) << 32) | height;
        if (logged_regions.size() < 32 && logged_regions.insert(key).second) {
            LOG_WARNING(Render_Vulkan,
                "RES-COMP: shader {:#x} sampled region {}x{} in allocation {}x{}, pitch {} at {:#x}",
                stage.pgm_hash, width, height, scene_width, scene_height,
                target->info.pitch, sharp.Address());
        }
    }
    return true;
}

/// Logs the first few times and then every 1000th.
bool Sample(std::atomic<u32>& counter) {
    const u32 n = ++counter;
    return n <= 8 || n % 1000 == 0;
}

// What vk_rasterizer.cpp's ClassifyDraw decides, from the registers alone.
enum class Filter { Draw, EliminateFastClear, FmaskDecompress, Resolve, NoPrimitive, Copy };

Filter Classify(const AmdGpu::Regs& regs) {
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        return Filter::EliminateFastClear;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        return Filter::FmaskDecompress;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        return Filter::Resolve;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        return Filter::NoPrimitive;
    }
    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const bool depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const bool stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        return Filter::Copy;
    }
    return Filter::Draw;
}

// As vk_rasterizer.cpp's MakeUserData.
Shader::PushData UserData(const AmdGpu::Regs& regs) {
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    push_data.quad_sample_mask = {regs.reg_array[0xa30e], regs.reg_array[0xa30f]}; // PA_SC_AA_MASK
    if (RytyGhost::ShouldSmooth(regs)) {
        push_data.quad_sample_mask = {0xffffffffu, 0xffffffffu};
    }
    return push_data;
}

// As vk_rasterizer.cpp's GetDrawOffsets.
std::pair<u32, u32> DrawOffsets(const AmdGpu::Regs& regs, const Shader::Info& info,
                                const Shader::Gcn::FetchShaderData& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (!fetch_shader.Empty()) {
        if (vertex_offset == 0 && fetch_shader.vertex_offset_sgpr != -1) {
            vertex_offset = info.user_data[fetch_shader.vertex_offset_sgpr];
        }
        if (fetch_shader.instance_offset_sgpr != -1) {
            instance_offset = info.user_data[fetch_shader.instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

// PERF-028's quad corners (vk_rasterizer.cpp).
constexpr std::array<u32, 6> QuadFirstVertex{0, 1, 2, 0, 2, 3};
constexpr std::array<u32, 6> QuadLastVertex{0, 1, 3, 1, 2, 3};
constexpr u64 MaxQuadIndexBytes = 16ULL << 20;

template <typename T>
u32 ExpandQuads(const T* in, u32 count, const std::optional<T> restart,
                const std::array<u32, 6>& corners, T* out) {
    u32 written = 0;
    if (!restart) {
        for (u32 quad = 0; quad + 4 <= count; quad += 4) {
            for (const u32 corner : corners) {
                out[written++] = in[quad + corner];
            }
        }
        return written;
    }
    std::array<T, 4> quad{};
    u32 filled = 0;
    for (u32 i = 0; i < count; ++i) {
        if (in[i] == *restart) {
            filled = 0;
            continue;
        }
        quad[filled++] = in[i];
        if (filled == 4) {
            for (const u32 corner : corners) {
                out[written++] = quad[corner];
            }
            filled = 0;
        }
    }
    return written;
}

// The texture cache's image usage, features and type (texture_cache/image.cpp), so native
// images are made as its images are.
vk::ImageUsageFlags UsageOf(const Instance& instance, const VideoCore::ImageInfo& info) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eSampled;
    if (!info.props.is_block) {
        if (instance.IsAttachmentFeedbackLoopLayoutSupported()) {
            usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
        }
        if (info.props.is_depth) {
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        } else {
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
            if (info.num_samples == 1 || instance.IsMultisampleStorageImageSupported()) {
                usage |= vk::ImageUsageFlagBits::eStorage;
            }
        }
    } else {
        usage |= vk::ImageUsageFlagBits::eStorage;
    }
    return usage;
}

vk::FormatFeatureFlags2 FeaturesOf(vk::ImageUsageFlags usage) {
    vk::FormatFeatureFlags2 features{};
    if (usage & vk::ImageUsageFlagBits::eTransferSrc) {
        features |= vk::FormatFeatureFlagBits2::eTransferSrc;
    }
    if (usage & vk::ImageUsageFlagBits::eTransferDst) {
        features |= vk::FormatFeatureFlagBits2::eTransferDst;
    }
    if (usage & vk::ImageUsageFlagBits::eSampled) {
        features |= vk::FormatFeatureFlagBits2::eSampledImage;
    }
    if (usage & vk::ImageUsageFlagBits::eColorAttachment) {
        features |= vk::FormatFeatureFlagBits2::eColorAttachment;
    }
    if (usage & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        features |= vk::FormatFeatureFlagBits2::eDepthStencilAttachment;
    }
    return features;
}

vk::ImageType TypeOf(AmdGpu::ImageType type) {
    return type == AmdGpu::ImageType::Color3D ? vk::ImageType::e3D : vk::ImageType::e2D;
}

u32 RhiUsage(vk::ImageUsageFlags usage) {
    u32 result = 0;
    if (usage & vk::ImageUsageFlagBits::eSampled) {
        result |= Native::Rhi::ImageUsageSampled;
    }
    if (usage & vk::ImageUsageFlagBits::eStorage) {
        result |= Native::Rhi::ImageUsageStorage;
    }
    if (usage & vk::ImageUsageFlagBits::eColorAttachment) {
        result |= Native::Rhi::ImageUsageColorTarget;
    }
    if (usage & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        result |= Native::Rhi::ImageUsageDepthTarget;
    }
    if (usage & vk::ImageUsageFlagBits::eTransferSrc) {
        result |= Native::Rhi::ImageUsageTransferSrc;
    }
    if (usage & vk::ImageUsageFlagBits::eTransferDst) {
        result |= Native::Rhi::ImageUsageTransferDst;
    }
    return result | Native::Rhi::ImageUsageMutableFormat;
}

template <typename T>
T* Handle(u64 handle) {
    return reinterpret_cast<T*>(handle);
}

/// NAT-110: adds the time until the end of the scope to a counter.
// OPT-001: the phase timers read the clock on one draw in 16 (reading it in every phase of every
// draw was 7% of the recorder's time); the report divides by the draws timed.
thread_local bool t_timed = true;
struct PhaseClock {
    u64* total;
    std::chrono::steady_clock::time_point start{};
    explicit PhaseClock(u64& total_) : total{t_timed ? &total_ : nullptr} {
        if (total) {
            start = std::chrono::steady_clock::now();
        }
    }
    ~PhaseClock() {
        if (total) {
            *total += std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
        }
    }
};
// Diagnostic-only nested interval. Finish permits a precise boundary while
// the destructor also records early returns; a completed sample is added once.
struct RecordPartitionClock {
    RecordPartitionMetric* metric{};
    std::chrono::steady_clock::time_point start{};
    explicit RecordPartitionClock(RecordPartitionMetric* value) : metric{t_timed ? value : nullptr} {
        if(metric)start=std::chrono::steady_clock::now();
    }
    void Finish() noexcept {
        if(!metric)return;
        metric->wall_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        ++metric->count;metric=nullptr;
    }
    ~RecordPartitionClock(){Finish();}
};
thread_local RecordPartitionMetric* t_record_partition_end=nullptr;
struct RecordPartitionEndGuard {
    RecordPartitionMetric* previous;
    explicit RecordPartitionEndGuard(RecordPartitionMetric* current) noexcept : previous{t_record_partition_end} {t_record_partition_end=current;}
    ~RecordPartitionEndGuard(){t_record_partition_end=previous;}
};

constexpr u64 ChunkSize = 8_MB;
constexpr u32 SpliceAfter = 512; ///< commands per list before it is run and submitted

} // namespace

bool NativeModeRequested() {
    // NAT-160: the launcher's renderer choice (GPU.native_renderer in the game's settings, so
    // GTSport.exe started on its own follows it too); RYGT_NATIVE=1 or =0 overrides it.
    static const bool requested = [] {
        if (const char* env = std::getenv("RYGT_NATIVE"); env && *env) {
            return env[0] == '1';
        }
        const auto value = Common::RytyGameConfigValue("native_renderer");
        return value && *value == "true";
    }();
    return requested;
}

void NativeRenderer::DrawBindings::PrepareDescriptorKeys(const Pipeline& pipeline) {
    if (!groups_ready) PrepareWriteGroups();
    for (u32 set = 0; set < grouped_writes.size(); ++set) {
        if (grouped_writes[set].empty() || set == pipeline.PushSet()) continue;
        descriptor_keys[set].Build<false>(pipeline.SetLayout(set), grouped_writes[set]);
        auto& key = descriptor_keys[set];
        if (key.cacheable) key.hash = Native::Parallel::AdaptiveXXH3(key.words.data(), key.words.size() * sizeof(u64));
    }
    key_layout = pipeline.GetLayout();
}

void NativeRenderer::DrawBindings::PrepareWriteGroups() {
    grouped_writes = GroupDescriptorWrites<vk::WriteDescriptorSet>(
        std::span<const vk::WriteDescriptorSet>{writes.data(), writes.size()},
        std::span<const u8>{write_sets.data(), write_sets.size()}, grouped_storage);
    groups_ready = true;
}

void NativeRenderer::DrawBindings::Clear() {
    writes.clear();
    write_sets.clear();
    grouped_writes = {};
    groups_ready = false;
    key_layout = nullptr;
    buffers.clear();
    images.clear();
    push = {};
    sampled.clear();
    stored.clear();
    gpu_writes.clear();
    fill_pattern.reset();
    fill_arguments_snapshot.reset();
    fill_address = 0;
    fill_size = 0;
    writes_buffers = false;
    failed = false;
}

struct NativeRenderer::FsrComposition {
    std::unique_ptr<Native::Rhi::Image> image;
    vk::UniqueImageView view;
    vk::Extent2D size{};
    vk::Format format{};
};

GtsSceneImage NativeRenderer::TemporalImage(const Native::ImageEntry& entry) {
    auto aspects = entry.info.props.is_depth ? vk::ImageAspectFlags{vk::ImageAspectFlagBits::eDepth}
                                            : vk::ImageAspectFlags{vk::ImageAspectFlagBits::eColor};
    if (entry.info.pixel_format == vk::Format::eD32SfloatS8Uint ||
        entry.info.pixel_format == vk::Format::eD24UnormS8Uint)
        aspects |= vk::ImageAspectFlagBits::eStencil;
    return {entry.info, entry.image->NativeHandle(),
            vk::Image{Handle<VkImage_T>(entry.image->NativeHandle())}, aspects,
            [this, &entry](vk::Format format) {
                VideoCore::ImageViewInfo view{};
                view.format = format;
                return View(entry, view, ViewUse::Sampled);
            }, [this, &entry] { Written(&entry); }};
}

void NativeRenderer::CaptureNativeFsr(const GraphicsPipeline& pipeline, const Targets& targets,
                                     const DrawBindings& bindings) {
    if (!r.temporalDlss.Enabled()) return;
    const auto& regs = r.liverpool->DrawRegs();
    const auto* fs = pipeline.GetStages()[u32(Shader::SwStage::Fragment)];
    if (!fs || regs.num_indices > 6 ||
        regs.num_instances.NumInstances() != 1 || targets.state.num_color_attachments != 2 ||
        bindings.sampled.size() != 4 || !targets.colors[0] || !targets.colors[1]) { static u32 rejected{}; if (rejected++ < 3) LOG_INFO(Render_Vulkan, "NATIVE-FSR: capture gate 1, fs {:#x}", fs ? fs->pgm_hash : 0); return; }
    const auto* history_out = targets.colors[0];
    const auto* resolved = targets.colors[1];
    const vk::Extent2D size{resolved->info.size.width, resolved->info.size.height};
    if (history_out->info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        resolved->info.pixel_format != vk::Format::eB10G11R11UfloatPack32 ||
        history_out->info.size.width != size.width || history_out->info.size.height != size.height)
        { static u32 rejected{}; if (rejected++ < 3) LOG_INFO(Render_Vulkan, "NATIVE-FSR: capture gate 2, fs {:#x}", fs ? fs->pgm_hash : 0); return; }
    const Native::ImageEntry *color{}, *depth{}, *motion{}, *history{};
    for (const auto* e : bindings.sampled) {
        const auto& i = e->info;
        if (i.size.width != size.width || i.size.height != size.height || i.num_samples != 1 ||
            i.resources.layers != 1 || i.resources.levels != 1) { static u32 rejected{}; if (rejected++ < 3) LOG_INFO(Render_Vulkan, "NATIVE-FSR: capture gate 3, fs {:#x}", fs ? fs->pgm_hash : 0); return; }
        // The sampled R32 descriptor may alias the real depth/stencil attachment.
        // TemporalImage preserves its actual aspects; Capture extracts D32 through
        // a buffer into FSR's separate R32 image instead of making a D32-only view.
        if (i.pixel_format == vk::Format::eR32Sfloat ||
            i.pixel_format == vk::Format::eD32Sfloat ||
            i.pixel_format == vk::Format::eD32SfloatS8Uint) depth = e;
        else if (i.pixel_format == vk::Format::eR16G16Sfloat) motion = e;
        else if (i.pixel_format == vk::Format::eB10G11R11UfloatPack32) color = e;
        else if (i.pixel_format == vk::Format::eR16G16B16A16Sfloat) history = e;
        else { static u32 rejected{}; if (rejected++ < 3) LOG_INFO(Render_Vulkan, "NATIVE-FSR: capture gate 4, fs {:#x}", fs ? fs->pgm_hash : 0); return; }
    }
    if (!color || !depth || !motion || !history || color == resolved || history == history_out) { static u32 rejected{}; if (rejected++ < 3) LOG_INFO(Render_Vulkan, "NATIVE-FSR: capture gate 5, fs {:#x}", fs ? fs->pgm_hash : 0); return; }
    std::array<float, 28> constants{};
    bool valid = false;
    for (const auto& buffer : fs->buffers) {
        if (buffer.IsSpecial() || buffer.is_written) continue;
        const auto sharp = buffer.GetSharp(*fs);
        if (!sharp.base_address || sharp.GetSize() != sizeof(constants)) continue;
        if (!Core::RytyMemory::GuardedCopy(constants.data(),
                reinterpret_cast<const void*>(sharp.base_address), sizeof(constants))) { static u32 rejected{}; if (rejected++ < 3) LOG_INFO(Render_Vulkan, "NATIVE-FSR: capture gate 6, fs {:#x}", fs ? fs->pgm_hash : 0); return; }
        valid = std::ranges::all_of(constants, [](float v) { return std::isfinite(v); }) &&
            std::abs(constants[14] - size.width * .5f) <= .01f &&
            std::abs(constants[15] + size.height * .5f) <= .01f &&
            constants[16] == size.width && constants[17] == size.height &&
            std::abs(constants[20] * constants[14]) <= 1.f &&
            std::abs(constants[21] * constants[15]) <= 1.f;
        break;
    }
    if (!valid) { static u32 rejected{}; if (rejected++ < 3) LOG_INFO(Render_Vulkan, "NATIVE-FSR: capture gate 7, fs {:#x}", fs ? fs->pgm_hash : 0); return; }
    Splice();
    const auto captured_frame = g_prepared_frames.load(std::memory_order_relaxed);
    const bool captured = r.temporalDlss.Capture(captured_frame, TemporalImage(*color),
        TemporalImage(*depth), TemporalImage(*motion), TemporalImage(*resolved), constants);
    static u32 captured_logged{};
    if (captured_logged++ < 12)
        LOG_INFO(Render_Vulkan, "NATIVE-FSR: capture frame {} fs {:#x} accepted {} resolved {:#x}",
            captured_frame, fs->pgm_hash, captured, resolved->image->NativeHandle());
}

bool NativeRenderer::BeginNativeFsr(const GraphicsPipeline& pipeline, const Targets& targets,
                                   const DrawBindings& bindings) {
    if (!r.temporalDlss.Enabled()) return false;
    const auto* fs = pipeline.GetStages()[u32(Shader::SwStage::Fragment)];
    const auto& regs = r.liverpool->DrawRegs();
    if (!fs || !IsSceneCompositionShader(fs->pgm_hash) || regs.num_indices > 6 ||
        regs.num_instances.NumInstances() != 1 || targets.depth ||
        targets.state.num_color_attachments != 1 || !targets.colors[0] ||
        regs.blend_control[0].enable || regs.color_target_mask.GetMask(0) != 0xf) return false;
    const auto* target = targets.colors[0];
    const auto& desc = target->image->Desc();
    if (target->info.pixel_format != vk::Format::eA2B10G10R10UnormPack32 ||
        desc.samples != 1 || desc.layers != 1 || desc.mips != 1 ||
        targets.state.width != desc.width || targets.state.height != desc.height) return false;
    const auto now = g_prepared_frames.load(std::memory_order_relaxed);
    const auto scene = std::ranges::find_if(bindings.sampled, [&](const auto* e) {
        return r.temporalDlss.HasScene(now, e->image->NativeHandle());
    });
    if (scene == bindings.sampled.end()) return false;
    const auto size = r.temporalDlss.RenderSize();
    if (size.width > desc.width || size.height > desc.height) return false;
    const auto features = r.instance.GetPhysicalDevice().getFormatProperties(
        target->info.pixel_format).optimalTilingFeatures;
    const auto required = vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst |
                          vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
    if ((features & required) != required) return false;
    // Startup-only scene dimensions: retain the scratch allocation until renderer teardown.
    // Unexpected size/format changes fall back without destroying in-flight resources.
    if (fsr_composition && (fsr_composition->size != size ||
                           fsr_composition->format != target->info.pixel_format)) return false;
    if (!fsr_composition) {
        auto scratch = std::make_unique<FsrComposition>();
        auto scratch_desc = desc;
        scratch_desc.width = size.width; scratch_desc.height = size.height;
        scratch_desc.usage = Native::Rhi::ImageUsageSampled | Native::Rhi::ImageUsageTransferSrc |
                             Native::Rhi::ImageUsageTransferDst;
        scratch_desc.name = "Native FSR scene composition";
        scratch->image = device->CreateImage(scratch_desc);
        if (!scratch->image) return false;
        scratch->size = size; scratch->format = target->info.pixel_format;
        const vk::Image image{Handle<VkImage_T>(scratch->image->NativeHandle())};
        auto [result, view] = r.instance.GetDevice().createImageViewUnique({
            .image = image, .viewType = vk::ImageViewType::e2D, .format = scratch->format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
        if (result != vk::Result::eSuccess) return false;
        scratch->view = std::move(view);
        fsr_composition = std::move(scratch);
        PrepareImages();
    }
    Splice();
    r.temporalDlss.PrepareScene(now, TemporalImage(**scene));
    Begin();
    UiHires::Get().scale = 0.f;
    return true;
}

void NativeRenderer::FinishNativeFsr(const Targets& targets) {
    EndRendering();
    GlobalBarrier();
    auto& s = Begin();
    const auto* target = targets.colors[0];
    const vk::Image source{Handle<VkImage_T>(target->image->NativeHandle())};
    const vk::Image scratch{Handle<VkImage_T>(fsr_composition->image->NativeHandle())};
    const auto size = fsr_composition->size;
    const vk::ImageCopy copy{
        .srcSubresource{vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource{vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent{size.width, size.height, 1}};
    s.cmd.copyImage(source, vk::ImageLayout::eGeneral, scratch, vk::ImageLayout::eGeneral, copy);
    Splice();
    // This is a real scene-sized image, not a cropped view of the full-sized game target.
    auto info = target->info;
    info.size.width = size.width; info.size.height = size.height;
    GtsSceneImage scene{info, target->image->NativeHandle(), scratch,
        vk::ImageAspectFlagBits::eColor,
        [this](vk::Format) { return *fsr_composition->view; }, [] {}};
    const auto now = g_prepared_frames.load(std::memory_order_relaxed);
    if (r.temporalDlss.ResolveNativeComposition(now, scene, TemporalImage(*target))) {
        fsr_display_address = target->info.guest_address;
        fsr_display_frame = frame;
        fsr_display_backend = r.temporalDlss.BackendName();
        static u32 logged{};
        if (logged++ < 8)
            LOG_INFO(Render_Vulkan, "NATIVE-FSR: reconstructed {}x{} to {}x{}, composed into {}x{} before HUD",
                size.width, size.height, r.temporalDlss.OutputSize().width,
                r.temporalDlss.OutputSize().height, target->info.size.width, target->info.size.height);
    } else {
        // Preserve a complete output picture even if the optional provider fails.
        auto& fallback = Begin();
        GlobalBarrier();
        vk::ImageBlit blit{};
        blit.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.srcOffsets[1] = {s32(size.width), s32(size.height), 1};
        blit.dstOffsets[1] = {s32(target->info.size.width), s32(target->info.size.height), 1};
        fallback.cmd.blitImage(scratch, vk::ImageLayout::eGeneral, source,
                               vk::ImageLayout::eGeneral, blit, vk::Filter::eLinear);
        fallback.list->Barrier(Native::Rhi::ScopeAll, Native::Rhi::ScopeAll);
    }
    Written(target);
    Begin(); // Direct backend commands invalidate cached Native pipeline/dynamic state.
}

NativeRenderer::NativeRenderer(Rasterizer& rasterizer) : r{rasterizer} {
    sampler_cache_epoch.fetch_add(1, std::memory_order_acq_rel);
    if(PreparedSampledProofRequested())try {prepared_proof_contexts.Register(this);} catch(...) {}
    device = Native::Rhi::CreateVulkanDevice(r.instance);
    ASSERT_MSG(device, "NAT-100: no native device");
    const VkSemaphore semaphore = r.scheduler.GetWorkSemaphore()->Handle();
    timeline = device->ImportFence(reinterpret_cast<u64>(semaphore));
    device->UseExternalTimeline(timeline.get(), [this] { return r.scheduler.CurrentTick(); });
    LOG_INFO(Render_Vulkan, "DRAW-PREP: mask {} (1 range union, 2 region dedup, 4 thread-local stats, 8 batched diagnostic polling)", DrawPreparationMask());
    registry = std::make_unique<Native::ImageRegistry>(*device);
    registry->SetImageFactory([this](const VideoCore::ImageInfo& info, bool render_target) {
        return MakeImage(info, render_target);
    });
    registry->SetBackingValidator([this](VAddr address, u64 size) {
        return HasImageBacking(address, size);
    });
    registry->SetMemoryClamp([this](VAddr address, u64 size) -> u64 {
        Native::SlowCallTrace<3> trace{Native::trace_guest_hash_calls, "image-readable-prefix",
            {"valid-mapping", "clamp-area", "readable-prefix"}};
        trace.Next(0, address, size);
        // Mapped, and readable by the CPU (GPU-only memory is not: NAT-106).
        if (!r.memory->IsValidMapping(address)) {
            return 0;
        }
        trace.Next(1);
        const u64 mapped = r.memory->ClampRangeSize(address, size);
        trace.Next(2);
        return Native::ReadablePrefix(reinterpret_cast<const void*>(address), mapped);
    });
    registry->SetUploadSource([this](const Native::ImageEntry& entry,
                                     Native::Rhi::CommandList& list) {
        Native::UploadPhaseTrace<10> trace{t_timed || entry.info.guest_size >= 4_MB,
            "image-gpu-source-detail",
            {"region-lookup", "region-refresh", "wrap-region", "import-guest", "find-import",
             "gather-allocate", "gather-copies", "refresh-overlaps", "copy-overlaps", "cpu-stage-policy"}};
        // NAT-107: textures are uploaded from guest memory as the GPU sees it, in GPU order.
        Native::ImageRegistry::UploadSource source;
        auto* direct = r.buffer_cache.GetDirectMemory();
        const VAddr address = entry.info.guest_address;
        const u64 size = entry.info.guest_size;
        const auto cpu_source = [&]() {
            Native::ImageRegistry::UploadSource staged;
            if (size == 0 || size > 64_MB) return staged;
            Native::UploadPhaseTrace<8> detail{size >= 4_MB,"cpu-texture-source",
                {"allocate", "mapping", "readable-prefix", "zero-tail", "guarded-copy",
                 "sparse-copy", "wrap-buffer", "local-stage"}};
            detail.Next(0,address,size);
            const auto [buffer, offset, destination] = Allocate(size, r.instance.StorageMinAlignment());
            detail.Next(1);
            u64 readable = 0;
            if (r.memory->IsValidMapping(address)) {
                const u64 mapped = r.memory->ClampRangeSize(address, size);
                detail.Next(2);
                readable = Native::ReadablePrefix(reinterpret_cast<const void*>(address), mapped);
            }
            detail.Next(3);
            if (readable < size) std::memset(destination + readable, 0, size - readable);
            detail.Next(4);
            if (readable && !Core::RytyMemory::GuardedCopy(destination,
                    reinterpret_cast<const void*>(address), readable)) {
                detail.Next(5);
                r.memory->CopySparseMemory(address, destination, readable);
            }
            detail.Next(6);
            auto wrapped = device->WrapBuffer(reinterpret_cast<u64>(static_cast<VkBuffer>(buffer)),
                                               stream.chunks.back().buffer->Size());
            staged.buffer = wrapped.get();
            staged.offset = offset;
            staged.kind = 4;
            staged.keep.push_back(std::move(wrapped));
            detail.Next(7);
            return Native::LocalTiledSource(*registry,list,std::move(staged),size,
                                            entry.info.props.is_tiled,r.instance.GetVendorID());
        };
        trace.Next(0, address, size);
        if (!direct || size == 0) {
            return source;
        }
        // NAT-146: the GPU's bytes there are in a region (shader-written memory lives in video
        // memory): the upload reads the region.
        Region* overlap = RegionOver(address, size);
        if (Region* region = overlap;
            region && address >= region->begin && address + size <= region->end &&
            (address - region->begin) % r.instance.StorageMinAlignment() == 0) {
            trace.Next(1);
            RefreshRegion(*region, address, size);
            // RefreshRegion may record transfer writes after FlushUploads' initial barrier.
            // The upload or untiling dispatch must read those refreshed bytes.
            list.Barrier(Native::Rhi::ScopeTransfer,
                         Native::Rhi::ScopeTransfer | Native::Rhi::ScopeCompute);
            trace.Next(2);
            auto wrapped = device->WrapBuffer(region->buffer->NativeHandle(), region->buffer->Size());
            source.buffer = wrapped.get();
            source.offset = address - region->begin;
            source.kind = 1;
            // MakeRegion watches the complete range before any target is initialized from it.
            source.guest_writes_tracked = true;
            source.keep.push_back(std::move(wrapped));
            return source;
        }
        // Many large streamed textures span over a thousand 16 KiB host views.
        // Repeated import attempts cost 17-35 ms even after the first upload.
        // A never-GPU-written CPU texture can use RecordUploads' guarded staging
        // copy instead. Existing imports and every authoritative GPU source keep
        // the original path, including partial region overlays.
        if (entry.from_cpu && size > 4_MB && size <= 64_MB &&
            !entry.gpu_backing_written.load(std::memory_order_acquire) && !overlap &&
            r.instance.GetVendorID() == 0x10de) {
            trace.Next(9);
            Native::UploadPhaseTrace<5> policy_trace{true,"cpu-staging-eligibility",
                {"sources-lock", "gpu-history", "find-import", "readable-check", "cpu-source"}};
            policy_trace.Next(0,address,size);
            bool has_sources;
            {
                std::scoped_lock lock{const_cast<Native::ImageEntry&>(entry).sources_mutex};
                has_sources = !entry.sources.empty();
            }
            policy_trace.Next(1);
            const bool gpu_written = entry.write_version.load(std::memory_order_relaxed) != 0 ||
                                     entry.foreign_clock.load(std::memory_order_relaxed) != 0 ||
                                     entry.rewrite_clock != 0;
            policy_trace.Next(2);
            const bool single_import=direct->FindBuffer(address,size).has_value();
            policy_trace.Next(3);
            const bool cpu_readable=Native::Readable(reinterpret_cast<const void*>(address),size);
            if (Native::PreferCpuTextureStaging(size, true, true, true, gpu_written,
                    has_sources, false, single_import,cpu_readable)) {
                policy_trace.Next(4);
                static std::atomic<u64> staged{};
                const u64 count = ++staged;
                if (count <= 8 || count % 128 == 0) {
                    LOG_INFO(Render_Vulkan,
                             "UPLOAD-STAGING: CPU-only texture {:#x}+{:#x} avoids host import ({} total)",
                             address, size, count);
                }
                return cpu_source();
            }
        }
        trace.Next(3);
        if (!direct->EnsureImported(address, size)) {
            return cpu_source();
        }
        trace.Next(4);
        if (const auto found = direct->FindBuffer(address, size);
            !overlap && found && found->second % r.instance.StorageMinAlignment() == 0) {
            auto wrapped = device->WrapBuffer(
                reinterpret_cast<u64>(static_cast<VkBuffer>(found->first->Handle())),
                found->first->SizeBytes());
            source.buffer = wrapped.get();
            source.offset = found->second;
            source.kind = 2;
            source.keep.push_back(std::move(wrapped));
            return Native::LocalTiledSource(*registry,list,std::move(source),size,
                                            entry.info.props.is_tiled,r.instance.GetVendorID());
        }
        trace.Next(5);
        std::unique_ptr<Native::Rhi::Buffer> temporary_gather;
        auto* gathered = registry->AcquireUploadGatherScratch(list, size);
        if (!gathered) {
            temporary_gather = device->CreateBuffer({.size = size,
                                              .usage = Native::Rhi::BufferUsageStorage |
                                                       Native::Rhi::BufferUsageTransferSrc |
                                                       Native::Rhi::BufferUsageTransferDst,
                                              .memory = Native::Rhi::MemoryKind::DeviceLocal,
                                              .name = "native upload gather"});
            gathered = temporary_gather.get();
        }
        if (!gathered) {
            return source;
        }
        const vk::CommandBuffer cmd{Handle<VkCommandBuffer_T>(list.NativeHandle())};
        const vk::Buffer dst{Handle<VkBuffer_T>(gathered->NativeHandle())};
        trace.Next(6);
        const bool all = direct->ForEachPart(
            address, size, [&](const VideoCore::Buffer& view, u64 view_offset, u64 offset, u64 bytes) {
                cmd.copyBuffer(view.Handle(), dst,
                               vk::BufferCopy{.srcOffset = view_offset, .dstOffset = offset,
                                              .size = bytes});
            });
        if (all) {
            if (overlap) {
                trace.Next(7);
                // The texture can straddle several GPU-written regions and imported guest
                // memory. Keep guest bytes in the gaps, but use the GPU's current bytes for
                // every overlap. Reading the whole range from the import loses those writes.
                for (auto& [base, region] : regions) {
                    const VAddr begin = std::max(address, region.begin);
                    const VAddr end = std::min(address + size, region.end);
                    if (begin < end) {
                        RefreshRegion(region, begin, end - begin);
                    }
                }
                list.Barrier(Native::Rhi::ScopeDraw | Native::Rhi::ScopeCompute |
                                 Native::Rhi::ScopeTransfer,
                             Native::Rhi::ScopeTransfer);
                trace.Next(8);
                for (auto& [base, region] : regions) {
                    const VAddr begin = std::max(address, region.begin);
                    const VAddr end = std::min(address + size, region.end);
                    if (begin < end) {
                        cmd.copyBuffer(
                            vk::Buffer{Handle<VkBuffer_T>(region.buffer->NativeHandle())}, dst,
                            vk::BufferCopy{.srcOffset = begin - region.begin,
                                           .dstOffset = begin - address,
                                           .size = end - begin});
                    }
                }
            }
            list.Barrier(Native::Rhi::ScopeTransfer,
                         Native::Rhi::ScopeCompute | Native::Rhi::ScopeTransfer);
            source.buffer = gathered;
            source.kind = 3;
        }
        if (temporary_gather) source.keep.push_back(std::move(temporary_gather));
        return source;
    });
    registry->SetUploadHook([this](Native::ImageEntry& entry) {
        // NAT-105: CPU writes to a texture's memory from now on fault once, and the texture
        // is uploaded again.
        WatchPages(entry.info.guest_address, std::max<u64>(entry.info.guest_size, 1));
    });
    gds = device->CreateBuffer({.size = 64_KB,
                                .usage = Native::Rhi::BufferUsageStorage |
                                         Native::Rhi::BufferUsageTransferSrc |
                                         Native::Rhi::BufferUsageTransferDst,
                                .memory = Native::Rhi::MemoryKind::Upload,
                                .name = "native gds"});
    ASSERT_MSG(gds && gds->Mapped(), "NAT-100: no GDS buffer");
    std::memset(gds->Mapped(), 0, gds->Size());
    mirror_preparation = std::make_unique<AsyncMirrorBuffer<Native::Rhi::Buffer>>(
        [this](auto request) -> std::unique_ptr<Native::Rhi::Buffer> {
            if (request.generation != unmap_generation.load()) return {};
            if (auto* direct = r.buffer_cache.GetDirectMemory()) {
                direct->WarmImported(request.begin, request.end - request.begin);
            }
            if (request.generation != unmap_generation.load()) return {};
            return device->CreateBuffer({.size = request.end - request.begin,
                .usage = Native::Rhi::BufferUsageStorage | Native::Rhi::BufferUsageVertex |
                         Native::Rhi::BufferUsageIndex | Native::Rhi::BufferUsageTransferSrc |
                         Native::Rhi::BufferUsageTransferDst,
                .memory = Native::Rhi::MemoryKind::DeviceLocal,
                .name = "native mirror"});
        }, [] {
#ifdef _WIN32
            SetThreadDescription(GetCurrentThread(), L"shadGT:GeometryPrepare");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            SetThreadPriorityBoost(GetCurrentThread(), TRUE);
#endif
        });
    if(AheadRequested() && ParallelRecordingRequested() && !r.instance.IsIntel()) {
        try {auto context=std::make_shared<AheadContext>(r.instance.GetDevice());std::scoped_lock guard{ahead_context_mutex};ahead_contexts.emplace(this,context);}
        catch(...) {LOG_WARNING(Render_Vulkan,"PARALLEL-PRODUCER-DESC setup failed; using original descriptors");}
    }
    LOG_INFO(Render_Vulkan, "NAT-100: native mode: draws, dispatches and copies are recorded with "
                            "native images and guest memory read in place");
}

NativeRenderer::~NativeRenderer() {
    if(PreparedSampledProofRequested())prepared_proof_contexts.Close(this);
    auto ahead_shutdown=BeginAheadShutdown(this);
    if(ahead_shutdown)while(ahead_shutdown->active.load(std::memory_order_acquire) || !ahead_shutdown->pools.Quiescent())std::this_thread::yield();
    scene_indirect_buffer.reset();scene_indirect_args.clear();
    if(SceneTrace::state.pool){const auto pool=SceneTrace::state.pool;const auto device=r.instance.GetDevice();r.scheduler.DeferOperation([device,pool]{device.destroyQueryPool(pool);});SceneTrace::state={};}
    mirror_preparation.reset(); // Join before any device or direct-memory teardown.
    sampler_cache_epoch.fetch_add(1, std::memory_order_acq_rel);
    std::scoped_lock lock{mutex};
    if (auto* sidecar=FindParallelRuntime(this)) {
        sidecar->closing=true;
        EndRendering();Splice();
        const auto ahead_pending=[&] {return sidecar->ahead_live!=0;};
        if(sidecar->coordinator->InFlightNodes() || sidecar->sync_pins.Waiting() || ahead_pending()) {
            r.scheduler.Finish();
            while(sidecar->coordinator->InFlightNodes() || sidecar->sync_pins.Waiting() || ahead_pending()) {
                const u64 done=timeline->Completed();sidecar->coordinator->Collect(done);
                sidecar->sync_pins.Collect(done,[&](auto& callback) noexcept {return Native::Rhi::TryEnqueueCompleted(*device,callback);});
                for(auto& flight:sidecar->ahead_flights)if(flight.release && flight.tick && flight.tick<=done){
                    flight.lease.reset();if(!Native::Rhi::TryEnqueueCompleted(*device,flight.release))continue;flight.tick=0;--sidecar->ahead_live;}
                if(sidecar->coordinator->InFlightNodes() || sidecar->sync_pins.Waiting() || ahead_pending())std::this_thread::yield();
            }
        }
        std::lock_guard sidecar_lock(parallel_sidecars_mutex);
        parallel_sidecars_generation.fetch_add(1,std::memory_order_release);
        parallel_sidecars.erase(this); // joins workers before device/view destruction
    }
    if(ahead_shutdown) {
        {std::scoped_lock prepared_lock{prepared_pool_mutex};for(auto& prepared:prepared_all)if(prepared)prepared->ahead_descriptor_lease.store({},std::memory_order_release);}
        r.scheduler.Finish();
        while(!ahead_shutdown->pools.DestroyReady(timeline->Completed()))std::this_thread::yield();

    }
    const vk::Device vk_device = r.instance.GetDevice();
    for (const auto pool : free_pools) {
        vk_device.destroyDescriptorPool(pool);
    }
    for (const auto& [tick, pool] : retiring_pools) {
        vk_device.destroyDescriptorPool(pool);
    }
    for (const auto pool : stream.pools) {
        vk_device.destroyDescriptorPool(pool);
    }
    views.clear();
    samplers.clear();
}

Native::MadeImage NativeRenderer::MakeImage(const VideoCore::ImageInfo& info, bool) {
    Native::MadeImage made{};
    if (info.pixel_format == vk::Format::eUndefined) {
        return made;
    }
    const Instance& instance = r.instance;
    vk::ImageCreateFlags flags{vk::ImageCreateFlagBits::eMutableFormat |
                               vk::ImageCreateFlagBits::eExtendedUsage};
    if (r.instance.IsImageCubeArraySupported() && !info.props.is_volume &&
        info.size.width == info.size.height &&
        info.resources.layers >= 6 && info.num_samples == 1) {
        flags |= vk::ImageCreateFlagBits::eCubeCompatible;
    }
    if (info.props.is_volume) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance.Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (info.props.is_block && instance.IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }
    vk::ImageUsageFlags usage = UsageOf(instance, info);
    vk::FormatFeatureFlags2 features = FeaturesOf(usage);
    const vk::Format format = instance.GetSupportedFormat(info.pixel_format, features);
    const vk::ImageType type = TypeOf(info.type);
    vk::PhysicalDeviceImageFormatInfo2 format_info{
        .format = format,
        .type = type,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .flags = flags,
    };
    auto properties = instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
    // Compressed textures are sampled directly. Storage is optional for compatible
    // uncompressed views, and some drivers reject that combination for BC6H.
    // Keep the original usage when accepted, including NVIDIA's extended-usage path.
    if (properties.result == vk::Result::eErrorFormatNotSupported && info.props.is_block &&
        (usage & vk::ImageUsageFlagBits::eStorage)) {
        format_info.usage &= ~vk::ImageUsageFlagBits::eStorage;
        auto sampled = instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
        if (sampled.result == vk::Result::eSuccess) {
            usage = format_info.usage;
            features = FeaturesOf(usage);
            properties = std::move(sampled);
            static std::atomic<u32> fallbacks{};
            if (Sample(fallbacks)) {
                LOG_INFO(Render_Vulkan,
                         "NAT-100: compressed image {} uses sampled-only fallback at {:#x}",
                         vk::to_string(format), info.guest_address);
            }
        }
    }
    if (properties.result != vk::Result::eSuccess) {
        static std::atomic<u32> logged{};
        if (Sample(logged)) {
            LOG_ERROR(Render_Vulkan, "NAT-100: no image of format {} ({}x{}) at {:#x}: {}, usage={}, flags={}",
                      vk::to_string(format), info.size.width, info.size.height,
                      info.guest_address, vk::to_string(properties.result),
                      vk::to_string(usage), vk::to_string(flags));
        }
        return made;
    }
    const vk::ImageCreateInfo image_ci{
        .flags = flags,
        .imageType = type,
        .format = format,
        .extent{info.size.width, info.size.height, std::max(info.size.depth, 1u)},
        .mipLevels = std::max<u32>(info.resources.levels, 1),
        .arrayLayers = type == vk::ImageType::e3D ? 1u : std::max<u32>(info.resources.layers, 1),
        .samples = LiverpoolToVK::NumSamples(
            info.num_samples, properties.value.imageFormatProperties.sampleCounts),
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    Native::Rhi::ImageDesc desc{};
    desc.dim = type == vk::ImageType::e3D ? Native::Rhi::ImageDim::D3 : Native::Rhi::ImageDim::D2;
    desc.format = Native::Rhi::FormatFromVk(static_cast<u32>(format));
    desc.width = image_ci.extent.width;
    desc.height = image_ci.extent.height;
    desc.depth = image_ci.extent.depth;
    desc.mips = image_ci.mipLevels;
    desc.layers = image_ci.arrayLayers;
    desc.samples = static_cast<u32>(image_ci.samples);
    desc.usage = RhiUsage(usage);
    made.image = Native::Rhi::CreateVulkanImage(
        *device, static_cast<const VkImageCreateInfo&>(image_ci), desc);
    made.usage = static_cast<u32>(static_cast<VkImageUsageFlags>(usage));
    made.features = static_cast<u64>(static_cast<VkFormatFeatureFlags2>(features));
    return made;
}

vk::ImageView NativeRenderer::View(const Native::ImageEntry& entry,
                                   const VideoCore::ImageViewInfo& requested, ViewUse use) {
    std::scoped_lock lock{views_mutex};
    auto trace_view=[&](vk::ImageView handle,bool cached){
        if(!SceneTrace::Id())return;
        const auto& d=entry.image->Desc();const u32 mips=std::max(d.mips,1u),layers=d.dim==Native::Rhi::ImageDim::D3?1u:std::max(d.layers,1u);
        const u32 first=std::min<u32>(requested.range.base.level,mips-1),count=std::clamp<u32>(requested.range.extent.levels,1,mips-first);
        const u32 layer=std::min<u32>(requested.range.base.layer,layers-1),layer_count=std::clamp<u32>(requested.range.extent.layers,1,layers-layer);
        const float min_lod=requested.min_lod && r.instance.IsImageViewMinLodSupported()?std::min(float(requested.min_lod)/256.f,float(first+count-1)):0.f;
        r.FrameDumpEvent(fmt::format("scene-view id={} address={:#x} handle={:#x} use={} cached={} normalized_mip={}+{} normalized_layer={}+{} effective_min_lod={} min_lod_supported={} image_format={} requested_format={}",SceneTrace::Id(),entry.info.guest_address,reinterpret_cast<u64>(static_cast<VkImageView>(handle)),u32(use),cached,first,count,layer,layer_count,min_lod,r.instance.IsImageViewMinLodSupported(),u32(d.format),u32(requested.format)));
    };
    auto& records = views[&entry];
    for (const auto& record : records) {
        if (record.use == use && record.info == requested) {
            trace_view(*record.view,true);
            return *record.view;
        }
    }
    // As the texture cache's ImageView (texture_cache/image_view.cpp), on the native image.
    const auto& image_desc = entry.image->Desc();
    const vk::Image image{Handle<VkImage_T>(entry.image->NativeHandle())};
    const vk::Format image_format = static_cast<vk::Format>(image_desc.format);
    const bool volume = image_desc.dim == Native::Rhi::ImageDim::D3;
    VideoCore::ImageViewInfo info = requested;
    // A view never reaches past the image (a T# may name more levels or layers than the
    // layout the image was made for).
    const u32 mips = std::max<u32>(image_desc.mips, 1);
    const u32 layers = volume ? 1u : std::max<u32>(image_desc.layers, 1);
    info.range.base.level = std::min<u32>(info.range.base.level, mips - 1);
    info.range.extent.levels =
        std::clamp<u32>(info.range.extent.levels, 1, mips - info.range.base.level);
    info.range.base.layer = std::min<u32>(info.range.base.layer, layers - 1);
    info.range.extent.layers =
        std::clamp<u32>(info.range.extent.layers, 1, layers - info.range.base.layer);

    vk::ImageUsageFlags usage{entry.backend_usage};
    if (use != ViewUse::Storage) {
        usage &= ~vk::ImageUsageFlagBits::eStorage;
    }
    vk::ImageViewUsageCreateInfo usage_ci{.usage = usage};
    vk::ImageViewMinLodCreateInfoEXT min_lod_ci{};
    if (info.min_lod != 0 && r.instance.IsImageViewMinLodSupported()) {
        const float last_level =
            static_cast<float>(info.range.base.level + info.range.extent.levels - 1);
        min_lod_ci.minLod = std::min(static_cast<float>(info.min_lod) / 256.f, last_level);
        usage_ci.pNext = &min_lod_ci;
    }
    vk::Format format = info.format;
    vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;
    if (entry.info.props.is_depth) {
        // A depth image: attachments see depth (and stencil); a T# names R32/R16 for depth and
        // R8 for stencil.
        const vk::ImageAspectFlags all =
            entry.info.props.has_stencil
                ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
                : vk::ImageAspectFlagBits::eDepth;
        if (use == ViewUse::Attachment) {
            aspect = all;
        } else if (entry.info.props.has_stencil && !LiverpoolToVK::IsFormatDepthCompatible(format) &&
                   LiverpoolToVK::IsFormatStencilCompatible(format)) {
            aspect = vk::ImageAspectFlagBits::eStencil;
        } else {
            aspect = vk::ImageAspectFlagBits::eDepth;
        }
        format = image_format;
    } else {
        format = r.instance.GetSupportedFormat(format, vk::FormatFeatureFlags2{entry.backend_features});
    }
    vk::ImageViewType view_type = VideoCore::ConvertImageViewType(info.type);
    if (use == ViewUse::CubeSampled) {
        view_type = vk::ImageViewType::eCubeArray;
    }
    if (volume) {
        view_type = vk::ImageViewType::e3D;
    } else if (view_type == vk::ImageViewType::e3D) {
        view_type = info.range.extent.layers > 1 ? vk::ImageViewType::e2DArray
                                                 : vk::ImageViewType::e2D;
    } else if (view_type == vk::ImageViewType::e2D && info.range.extent.layers > 1) {
        view_type = vk::ImageViewType::e2DArray;
    }
    const vk::ImageViewCreateInfo view_ci{
        .pNext = &usage_ci,
        .image = image,
        .viewType = view_type,
        .format = format,
        .components = info.mapping == AmdGpu::IdentityMapping
                          ? vk::ComponentMapping{}
                          : LiverpoolToVK::ComponentMapping(info.mapping),
        .subresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = info.range.base.level,
            .levelCount = info.range.extent.levels,
            .baseArrayLayer = info.range.base.layer,
            .layerCount = info.range.extent.layers,
        },
    };
    auto [result, view] = r.instance.GetDevice().createImageViewUnique(view_ci);
    if (result != vk::Result::eSuccess) {
        static std::atomic<u32> logged{};
        if (Sample(logged)) {
            LOG_ERROR(Render_Vulkan, "NAT-100: no view {} of image {:#x} ({})",
                      vk::to_string(format), entry.info.guest_address, vk::to_string(result));
        }
        return {};
    }
    const vk::ImageView handle = *view;
    trace_view(handle,false);
    if(SceneTrace::Id())r.FrameDumpEvent(fmt::format("scene-view-created id={} handle={:#x} format={} aspect={} type={}",SceneTrace::Id(),reinterpret_cast<u64>(static_cast<VkImageView>(handle)),u32(format),u32(aspect),u32(view_type)));
    records.push_back({requested, use, std::move(view)});
    return handle;
}

vk::Sampler NativeRenderer::SamplerFor(const AmdGpu::Sampler& sharp,
                                       AmdGpu::BorderColorBuffer border, bool is_depth) {
    const u64 hash = XXH3_64bits(&sharp, sizeof(sharp)) ^
                     (is_depth ? 0x9E3779B97F4A7C15ULL : 0) ^ (u64(border.Address()) << 1);
    static thread_local Native::EpochValueMemo<vk::Sampler> memo;
    const auto epoch = sampler_cache_epoch.load(std::memory_order_acquire);
    if (const auto found = memo.Find(this, epoch, hash)) return *found;
    std::scoped_lock lock{sampler_mutex};
    auto& slot = samplers[hash];
    if (!slot) {
        slot = std::make_unique<VideoCore::Sampler>(r.instance, sharp, border, is_depth);
    }
    const auto handle = slot->Handle();
    memo.Store(this, epoch, hash, handle);
    return handle;
}

NativeRenderer::Stream& NativeRenderer::Begin() {
    Stream& s = stream;
    if (s.list) {
        return s;
    }
    UploadMemoFor(this).Clear(); // new list: old upload-buffer bindings cannot be reused
    if (r.instance.IsIntel()) {
        // FIX-188: Intel Arc (#65) records into the scheduler's own command buffer.
        r.scheduler.EndRendering();
        s.cmd = r.scheduler.DirectCommandBuffer();
        s.wrapped_list = device->WrapCommandBuffer(
            reinterpret_cast<u64>(static_cast<VkCommandBuffer>(s.cmd)));
        s.list = s.wrapped_list.get();
    } else {
        s.list = device->AcquireCommandList(Native::Rhi::CommandListLevel::Secondary);
        s.list->Begin();
        s.cmd = vk::CommandBuffer{Handle<VkCommandBuffer_T>(s.list->NativeHandle())};
    }
    s.rendering = false;
    s.dynamic.Invalidate();
    s.pipelines.Invalidate();
    s.vertex_state_known = false; // OPT-005
    s.commands = 0;
    // What was recorded before this list (earlier lists, the presenter) is finished first.
    GlobalBarrier();
    MakePendingMirrors(); // NAT-150: behind the barrier the list starts with anyway
    return s;
}

void NativeRenderer::EndRendering() {
    RecordPartitionClock end_partition{t_record_partition_end};
    auto* sidecar=FindParallelRuntime(this);
    if(sidecar)sidecar->descriptor_updates.Flush(r.instance.GetDevice(),PendingDescriptorUpdates::EndPass);
    if(sidecar)sidecar->FinishSerialEligibleRun();
    if (sidecar && sidecar->active) {
        auto& pass=*sidecar->active;
        ++sidecar->pass_size_hist[std::min<std::size_t>(8,pass.draws.size())];
        u64 coordinator_start=0,coordinator_end=0;const bool cpu_started=ParallelGateDiagEnabled() && DescriptorThreadCpu100ns(coordinator_start);
        stream.rendering=false; // the captured pass has not begun on stream.cmd
        bool queued=false;
        if((pass.draws.size()>=8 || ParallelSmallPassBatchEnabled()) && !sidecar->closing) {
            const auto reset=[&]() noexcept {stream.dynamic.Invalidate();stream.pipelines.Invalidate();stream.vertex_state_known=false;};
            if(Native::Parallel::RotateSegment(*sidecar->coordinator,*device,sidecar->epoch,
                    stream.list,stream.cmd,sidecar->lease,64*1024,reset,&sidecar->rotation_diag)) {
                std::size_t bytes=sizeof(pass)+pass.draws.capacity()*sizeof(Native::Parallel::Draw);
                for(const auto& draw:pass.draws) bytes+=draw.descriptor_bytes;
                try {queued=pass.draws.size()>=8 ? (sidecar->transfer_applied_mode.Enabled() ? sidecar->coordinator->TryPassTransfer(sidecar->epoch,pass,bytes) : sidecar->coordinator->TryPass(sidecar->epoch,pass,bytes)) : sidecar->coordinator->TrySmallPass(sidecar->epoch,pass,bytes);}
                catch(const std::bad_alloc&) {if(pass.draws.size()<8)++sidecar->small_admission_exceptions;else ++sidecar->large_admission_exceptions;queued=false;}
            }
        }
        if(queued) {++sidecar->queued;sidecar->sync_pins.Queued(sidecar->pin_slot);} else {
            ++sidecar->synchronous;
            ASSERT_MSG(Native::Parallel::RecordPass(stream.cmd,r.instance,pass),"parallel pass synchronous fallback failed");
            ASSERT_MSG(sidecar->sync_pins.Tag(sidecar->pin_slot,sidecar->epoch),"parallel synchronous owner pin failed");
            stream.dynamic.Invalidate();stream.pipelines.Invalidate();stream.vertex_state_known=false;
        }
        pass.draws.clear();sidecar->scratch_draws.swap(pass.draws);
        sidecar->pin_slot=Native::Parallel::SyncOwnerPins<>::Invalid;
        sidecar->active.reset();sidecar->lease.reset();
        if(cpu_started && DescriptorThreadCpu100ns(coordinator_end) && coordinator_end>=coordinator_start) {
            sidecar->coordinator_cpu_100ns+=coordinator_end-coordinator_start;++sidecar->coordinator_cpu_samples;
        }
        return;
    }
    if (stream.rendering) {
        stream.cmd.endRendering();
        stream.rendering = false;
    }
}

void NativeRenderer::BeginRendering(const RenderState& state) {
    Stream& s = stream;
    if (s.rendering && s.render == state) {
        return;
    }
    EndRendering();
    // As Scheduler::BeginRendering.
    std::array<vk::RenderingAttachmentInfo, 8> colors;
    for (u32 i = 0; i < state.num_color_attachments; ++i) {
        const auto& cb = state.color_attachments[i];
        colors[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }
    const auto& db = state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth{
        .imageView = db.image_view,
        .imageLayout = vk::ImageLayout::eGeneral,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{
                                         .depth = std::bit_cast<float>(db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil{
        .imageView = db.image_view,
        .imageLayout = vk::ImageLayout::eGeneral,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };
    s.cmd.beginRendering(vk::RenderingInfo{
        .renderArea{.offset = {0, 0}, .extent = {state.width, state.height}},
        .layerCount = state.num_layers,
        .colorAttachmentCount = state.num_color_attachments,
        .pColorAttachments = colors.data(),
        .pDepthAttachment = db.has_depth ? &depth : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil : nullptr,
    });
    s.rendering = true;
    s.render = state;
    ++stats.passes;
}

void NativeRenderer::GlobalBarrier() {
    Stream& s = stream;
    EndRendering();
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    s.cmd.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
    s.written.clear();
    s.buffers_written = false;
    s.attach_dirty.clear();
    s.read.clear();
    ++stats.barriers;
    FlushWritebacks();
}

void NativeRenderer::PassBarrier() {
    Stream& s = stream;
    EndRendering();
    constexpr auto stages = vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                            vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits2::eLateFragmentTests;
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = stages,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        .dstStageMask = stages,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                         vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
    };
    s.cmd.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
    s.attach_dirty.clear();
    ++stats.pass_barriers;
}

void NativeRenderer::FlushWritebacks() {
    if (writebacks.empty() || !stream.list) {
        return;
    }
    Stream& s = stream;
    auto* direct = r.buffer_cache.GetDirectMemory();
    const u64 alignment = r.instance.StorageMinAlignment();
    for (auto* entry : writebacks) {
        const VAddr address = entry->info.guest_address;
        const u64 size = entry->info.guest_size;
        if (!direct || !direct->EnsureImported(address, size)) {
            continue;
        }
        const auto found = direct->FindBuffer(address, size);
        if (!found || found->second % alignment != 0) {
            continue;
        }
        auto out = device->WrapBuffer(
            reinterpret_cast<u64>(static_cast<VkBuffer>(found->first->Handle())),
            found->first->SizeBytes());
        if (registry->RecordDownload(*s.list, *entry, *out, found->second)) {
            ++stats.writebacks;
            if (RegionOver(address, size)) {
                // Image writes are authoritative for their entire byte range. A CPU-page
                // delta cannot represent them: its saved baseline can already contain the
                // same image bytes while an older native region contains something else.
                // Order the image download before copying its exact overlap into each region.
                const vk::MemoryBarrier2 downloaded{
                    .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferRead |
                                     vk::AccessFlagBits2::eTransferWrite,
                };
                s.cmd.pipelineBarrier2(vk::DependencyInfo{
                    .memoryBarrierCount = 1,
                    .pMemoryBarriers = &downloaded,
                });
                for (auto& [base, region] : regions) {
                    const VAddr begin = std::max(address, region.begin);
                    const VAddr end = std::min(address + size, region.end);
                    if (begin >= end) {
                        continue;
                    }
                    s.cmd.copyBuffer(found->first->Handle(),
                                     vk::Buffer{Handle<VkBuffer_T>(region.buffer->NativeHandle())},
                                     vk::BufferCopy{.srcOffset = found->second + begin - address,
                                                    .dstOffset = begin - region.begin,
                                                    .size = end - begin});
                }

            }
            // NAT-146: regions over the picture's memory copy it in before their next use.
            if (Region* region = RegionOver(address, size)) {
                for (VAddr page = Common::AlignDown(std::max<VAddr>(address, region->begin), 4096);
                     page < std::min<VAddr>(address + size, region->end); page += 4096) {
                    region->dirty_pages.push_back(page);
                }
                region->dirty_sorted = false;
            }
            // NAT-150: so do mirrors.
            for (auto it = FirstMirror(address);
                 it != mirrors.end() && it->second.begin < address + size; ++it) {
                Mirror& mirror = it->second;
                for (VAddr page = Common::AlignDown(std::max<VAddr>(address, mirror.begin), 4096);
                     page < std::min<VAddr>(address + size, mirror.end); page += 4096) {
                    mirror.dirty_pages.push_back(page);
                }
            }
        }
        s.keep.push_back(std::move(out));
    }
    writebacks.clear();
    s.pipelines.Invalidate(); // the re-tiling binds its own pipeline
    // What reads guest memory next (shaders, the CPU after the GPU) sees the pictures.
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    s.cmd.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

void NativeRenderer::PrepareImages() {
    Native::SlowCallTrace<6> trace{t_timed, "prepare-images",
        {"pending-init-lock", "pending-upload-lock", "end-rendering", "initialize", "global-barrier", "uploads"}};
    const bool init = device->PendingImageInit();
    trace.Next(1);
    const bool uploads = registry->HasPendingUploads();
    if (!init && !uploads) {
        return;
    }
    Stream& s = stream;
    trace.Next(2);
    EndRendering();
    if (init) {
        trace.Next(3);
        device->RecordPendingImageInit(*s.list);
    }
    if (uploads) {
        trace.Next(4);
        GlobalBarrier(); // NAT-107: uploads read guest memory as earlier GPU work left it
        trace.Next(5);
        registry->RecordUploads(*s.list, t_timed);
        s.pipelines.Invalidate(); // the uploads bind their own pipelines
    }
}

void NativeRenderer::ChunkFlushCheck() {
    // Long lists are run and submitted as they grow, so the GPU starts on them (as the old
    // path's SubmitChunkIfNeeded does).
    if (stream.commands >= SpliceAfter) {
        Splice();
        r.scheduler.Flush();
    }
}

NativeRenderer::Chunk NativeRenderer::TakeChunk(u64 size) {
    Native::SlowCallTrace<6> trace{t_timed, "take-upload-chunk",
        {"mutex", "timeline-query", "trim-free", "reclaim-retired", "take-free", "create-buffer"}};
    trace.Next(0, 0, size);
    auto* semaphore = r.scheduler.GetWorkSemaphore();
    std::scoped_lock lock{chunk_mutex};
    trace.Next(1);
    const auto has_available = [&] {
        return std::ranges::any_of(free_chunks, [&](const Chunk& chunk) {
            return chunk.buffer->Size() >= size;
        });
    };
    if (!has_available() && !retiring_chunks.empty()) {
        semaphore->Refresh();
    }
    const u64 known = semaphore->KnownGpuTick();
    // Loading can briefly need hundreds of MiB of upload chunks. Keep a bounded
    // reserve once their GPU work finishes instead of retaining that peak forever.
    trace.Next(2);
    trace.Next(3);
    CacheCompletedUploadChunks(free_chunks, retiring_chunks, known);
    trace.Next(4);
    auto fit = free_chunks.end();
    for (auto it = free_chunks.begin(); it != free_chunks.end(); ++it) {
        if (it->buffer->Size() >= size &&
            (fit == free_chunks.end() || it->buffer->Size() < fit->buffer->Size())) fit = it;
    }
    if (fit != free_chunks.end()) {
        Chunk chunk = std::move(*fit);
        free_chunks.erase(fit);
        return chunk;
    }
    Chunk chunk;
    trace.Next(5);
    chunk.buffer = device->CreateBuffer({
        .size = UploadChunkCapacity(size, ChunkSize),
        .usage = Native::Rhi::BufferUsageStorage | Native::Rhi::BufferUsageUniform |
                 Native::Rhi::BufferUsageIndex | Native::Rhi::BufferUsageVertex |
                 Native::Rhi::BufferUsageIndirect | Native::Rhi::BufferUsageTransferSrc,
        .memory = Native::Rhi::MemoryKind::Upload,
        .name = "native upload",
    });
    ASSERT_MSG(chunk.buffer && chunk.buffer->Mapped(), "NAT-100: no upload memory ({} bytes)",
               size);
    return chunk;
}

std::tuple<vk::Buffer, u64, u8*> NativeRenderer::Allocate(u64 size, u64 align) {
    Stream& s = stream;
    align = std::max<u64>(align, 16);
    if (s.chunks.empty() ||
        Common::AlignUp(s.chunk_used, align) + size > s.chunks.back().buffer->Size()) {
        s.chunks.push_back(TakeChunk(size));
        s.chunk_used = 0;
    }
    const auto& buffer = *s.chunks.back().buffer;
    const u64 offset = Common::AlignUp(s.chunk_used, align);
    s.chunk_used = offset + size;
    return {vk::Buffer{Handle<VkBuffer_T>(buffer.NativeHandle())}, offset, buffer.Mapped() + offset};
}

vk::DescriptorBufferInfo NativeRenderer::UploadFlatData(u32 stage, const u8* source, u32 size) {
    Native::SlowCallTrace<5> trace{t_timed, "upload-flat-data",
        {"compare", "allocate", "copy-upload", "copy-reference", "finish"}};
    trace.Next(0, reinterpret_cast<u64>(source), size);
    auto& memo = UploadMemoFor(this);
    const std::span<const u8> bytes{source, size};
    const auto lookup = memo.Find(stage, bytes, [](const void* data, size_t length) {
        return Native::Parallel::AdaptiveXXH3(data, length);
    });
    if (lookup.binding) {
        ++stats.flat_upload_hits;
        stats.flat_upload_bytes_saved += size;
        return *lookup.binding;
    }
    ++stats.flat_upload_misses;
    trace.Next(1);
    const auto [buffer, offset, data] = Allocate(size, r.instance.StorageMinAlignment());
    trace.Next(2);
    std::memcpy(data, source, size);
    const vk::DescriptorBufferInfo binding{buffer, offset, size};
    trace.Next(3);
    memo.Store(lookup, stage, bytes, binding);
    trace.Next(4);
    return binding;
}

vk::DescriptorPool NativeRenderer::TakeDescriptorPool() {
    const vk::Device vk_device = r.instance.GetDevice();
    vk::DescriptorPool ready;
    bool reset = false;
    {
        std::scoped_lock lock{chunk_mutex};
        std::tie(ready, reset) = Native::TakeReadyDescriptorPool(free_pools, retiring_pools,
            r.scheduler.GetWorkSemaphore()->KnownGpuTick());
    }
    if (ready) {
        if (reset) std::ignore = vk_device.resetDescriptorPool(ready);
        return ready;
    }
    static constexpr std::array<vk::DescriptorPoolSize, 4> sizes{{
        {vk::DescriptorType::eStorageBuffer, 8192},
        {vk::DescriptorType::eSampledImage, 8192},
        {vk::DescriptorType::eStorageImage, 1024},
        {vk::DescriptorType::eSampler, 4096},
    }};
    const auto [result, pool] = vk_device.createDescriptorPool(vk::DescriptorPoolCreateInfo{
        .maxSets = 512,
        .poolSizeCount = static_cast<u32>(sizes.size()),
        .pPoolSizes = sizes.data(),
    });
    ASSERT_MSG(result == vk::Result::eSuccess, "NAT-100: no descriptor pool");
    return pool;
}

bool NativeRenderer::HasImageBacking(VAddr address, u64 size) {
    // GPU-written regions are authoritative even without complete CPU backing.
    // Require containment, not just overlap; malformed spans still fail closed.
    constexpr VAddr MaxGpuAddress = 0x10000000000;
    if (!address || !size || address >= MaxGpuAddress || size > MaxGpuAddress - address) {
        return false;
    }
    {
        std::shared_lock lock{buffer_map_mutex};
        auto it = regions.upper_bound(address);
        if (it != regions.begin()) {
            const auto& region = std::prev(it)->second;
            if (address >= region.begin && address < region.end &&
                size <= region.end - address) {
                return true;
            }
        }
    }
    return Core::RytyMemory::IsValidMapping(address, size);
}

NativeRenderer::Region* NativeRenderer::RegionOver(VAddr address, u64 size) {
    auto it = regions.upper_bound(address);
    if (it != regions.begin()) {
        Region& region = std::prev(it)->second;
        if (address < region.end) {
            return &region;
        }
    }
    if (it != regions.end() && it->second.begin < address + size) {
        return &it->second;
    }
    return nullptr;
}

NativeRenderer::Region* NativeRenderer::MakeRegion(VAddr address, u64 size) {
    ++buffer_generation; // OPT-003: regions are taken in or made
    auto* direct = r.buffer_cache.GetDirectMemory();
    // The new region takes in every region it overlaps (their GPU-written contents kept).
    VAddr begin = Common::AlignDown(address, 4096);
    VAddr end = Common::AlignUp(address + size, 4096);
    std::vector<Region> merged;
    {
        std::unique_lock map_lock{buffer_map_mutex}; // OPT-010
        for (auto it = regions.begin(); it != regions.end();) {
            if (it->second.begin < end && it->second.end > begin) {
                begin = std::min(begin, it->second.begin);
                end = std::max(end, it->second.end);
                retired_watch_ranges.emplace_back(it->second.begin, it->second.end);
                merged.push_back(std::move(it->second));
                it = regions.erase(it);
            } else {
                ++it;
            }
        }
    }
    buffer_log.Note(begin, end); // OPT-010
    if (!direct || end - begin > 1_GB || !r.IsMappedStart(begin) ||
        !r.memory->IsValidGpuMapping(begin, end - begin)) {
        return nullptr;
    }
    // GPU-only guest allocations need not have CPU-importable backing. They still need
    // storage for shader writes (including the terrain's persistent shadow map).
    std::vector<Native::RegionInitRange> covered;
    covered.reserve(merged.size());
    for (const auto& old : merged) covered.push_back({old.begin,old.end});
    const auto gaps = Native::RegionInitGaps(begin,end,covered);
    u64 guest_init_bytes = 0;
    for (const auto gap : gaps) guest_init_bytes += gap.end-gap.begin;
    Region region;
    region.begin = begin;
    region.end = end;
    region.buffer = device->CreateBuffer({.size = end - begin,
                                          .usage = Native::Rhi::BufferUsageStorage |
                                                   Native::Rhi::BufferUsageVertex |
                                                   Native::Rhi::BufferUsageIndex |
                                                   Native::Rhi::BufferUsageIndirect |
                                                   Native::Rhi::BufferUsageTransferSrc |
                                                   Native::Rhi::BufferUsageTransferDst,
                                          .memory = Native::Rhi::MemoryKind::DeviceLocal,
                                          .name = "native region"});
    if (!region.buffer) {
        return nullptr;
    }
    Stream& s = Begin();
    GlobalBarrier();
    const vk::Buffer dst{Handle<VkBuffer_T>(region.buffer->NativeHandle())};
    // Initialize only uncovered guest spans. The covered bytes come from the
    // authoritative old GPU buffers below, including their pending CPU dirty pages.
    for (const auto gap : gaps) {
        const u64 gap_size = gap.end-gap.begin;
        bool imported = direct->EnsureImported(gap.begin,gap_size);
        if (imported) {
            imported = direct->ForEachPart(gap.begin,gap_size,
                [&](const VideoCore::Buffer& view,u64 view_offset,u64 offset,u64 bytes) {
                    s.cmd.copyBuffer(view.Handle(),dst,vk::BufferCopy{
                        .srcOffset=view_offset,.dstOffset=gap.begin-begin+offset,.size=bytes});
                });
        }
        if (!imported) {
            // A mapping can disappear after EnsureImported. Order any partial copies
            // before the complete sparse fallback for this gap.
            s.list->Barrier(Native::Rhi::ScopeTransfer,Native::Rhi::ScopeTransfer);
            for (u64 offset=0;offset<gap_size;) {
                const u64 bytes=std::min<u64>(4_MB,gap_size-offset);
                const auto [upload,upload_offset,data]=Allocate(bytes,4);
                r.memory->CopySparseMemory(gap.begin+offset,data,bytes);
                s.cmd.copyBuffer(upload,dst,vk::BufferCopy{
                    .srcOffset=upload_offset,.dstOffset=gap.begin-begin+offset,.size=bytes});
                offset+=bytes;
            }
        }
    }
    // Retain the existing transfer dependency before restoring authoritative data.
    // Planned guest gaps and old GPU ranges are disjoint.
    if (!merged.empty()) {
        s.list->Barrier(Native::Rhi::ScopeTransfer, Native::Rhi::ScopeTransfer);
    }
    for (auto& old : merged) {
        s.cmd.copyBuffer(vk::Buffer{Handle<VkBuffer_T>(old.buffer->NativeHandle())}, dst,
                         vk::BufferCopy{.srcOffset = 0,
                                        .dstOffset = old.begin - begin,
                                        .size = old.end - old.begin});
        s.keep.push_back(std::move(old.buffer)); // freed once the list ran
        // FIX-173: CPU writes to it not copied in yet are copied in at the new one's next use.
        region.dirty_pages.insert(region.dirty_pages.end(), old.dirty_pages.begin(),
                                  old.dirty_pages.end());
        region.dirty_sorted = false;
    }
    s.buffers_written = true;
    // CPU writes to it from now on are copied in before its next use.
    WatchPages(begin, end - begin);
    DropMirrors(begin, end - begin); // NAT-150: reads there use the region from now on
    ++stats.regions_made;
    if (end-begin >= 4_MB && !merged.empty()) {
        LOG_INFO(Render_Vulkan,
            "NAT-REGION-INIT: begin={:#x} bytes={} guest-init={} gpu-retained={} gaps={} old-regions={}",
            begin,end-begin,guest_init_bytes,end-begin-guest_init_bytes,gaps.size(),merged.size());
    }
    LOG_INFO(Render_Vulkan, "NAT-114: region {:#x}+{:#x} on the GPU ({} taken in)", begin,
             end - begin, merged.size());
    std::unique_lock map_lock{buffer_map_mutex}; // OPT-010
    auto [it, inserted] = regions.emplace(begin, std::move(region));
    return &it->second;
}

void NativeRenderer::SortRegionWrites() {
    if (!has_pending_region_writes.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    std::vector<std::pair<VAddr, VAddr>> writes;
    {
        std::scoped_lock lock{region_dirty_mutex};
        writes.swap(pending_region_writes);
    }
    for (const auto& [begin, end] : writes) {
        for (auto& [base, region] : regions) {
            if (region.begin < end && region.end > begin) {
                for (VAddr page = std::max(begin, region.begin); page < std::min(end, region.end);
                     page += 4096) {
                    region.dirty_pages.push_back(page);
                }
                region.dirty_sorted = false;
            }
        }
        // FIX-173: copies of pages no region holds are not needed.
        if (end - begin <= 64_KB) {
            std::scoped_lock lock{region_dirty_mutex};
            for (VAddr page = begin; page < end; page += 4096) {
                if (!RegionOver(page, 4096)) {
                    page_snapshots.erase(page);
                }
            }
        }
        // NAT-150: mirrors copy the pages in before their next use, unless the CPU wrote that
        // granule in another recent frame too: then the mirror goes and the granule is read in
        // place for a while.
        for (auto it = FirstMirror(begin); it != mirrors.end() && it->second.begin < end;) {
            Mirror& mirror = it->second;
            const VAddr from = std::max(begin, mirror.begin);
            const VAddr to = std::min(end, mirror.end);
            bool dynamic = false;
            for (u64 granule = from >> 16; granule <= (to - 1) >> 16; ++granule) {
                const auto [written, first] = mirror_writes.try_emplace(granule, frame);
                if (!first && written->second != frame && frame - written->second < 600) {
                    dynamic = true;
                    dynamic_granules[granule] = frame;
                }
                written->second = frame;
            }
            // FIX-184 (diagnosis, RYGT_NATIVE_MIRROR_NODROP=1): CPU-written mirrors stay (their
            // pages are copied in at the next use as usual).
            static const bool no_drop = [] {
                const char* env = std::getenv("RYGT_NATIVE_MIRROR_NODROP");
                return env && env[0] == '1';
            }();
            if (dynamic && no_drop) {
                dynamic = false;
            }
            if (dynamic) {
                mirror_bytes -= mirror.end - mirror.begin;
                ++stats.mirrors_dropped;
                ++buffer_generation; // OPT-003
                buffer_log.Note(mirror.begin, mirror.end); // OPT-010
                retired_watch_ranges.emplace_back(mirror.begin, mirror.end);
                std::unique_lock map_lock{buffer_map_mutex};
                it = mirrors.erase(it); // freed once the GPU is done with it
                continue;
            }
            for (VAddr page = from; page < to; page += 4096) {
                mirror.dirty_pages.push_back(page);
            }
            ++it;
        }
    }
    if (mirror_writes.size() > 1'000'000) {
        mirror_writes.clear();
    }
}

std::map<VAddr, NativeRenderer::Mirror>::iterator NativeRenderer::FirstMirror(VAddr begin) {
    auto it = mirrors.upper_bound(begin);
    if (it != mirrors.begin() && std::prev(it)->second.end > begin) {
        --it;
    }
    return it;
}

NativeRenderer::Mirror* NativeRenderer::MirrorHolding(VAddr address, u64 size) {
    auto it = mirrors.upper_bound(address);
    if (it == mirrors.begin()) {
        return nullptr;
    }
    Mirror& mirror = std::prev(it)->second;
    return address >= mirror.begin && address + size <= mirror.end ? &mirror : nullptr;
}

bool NativeRenderer::IsDynamic(VAddr begin, VAddr end) {
    constexpr u64 DynamicFrames = 600; // then the granule may be mirrored again
    if (dynamic_granules.empty()) {
        return false;
    }
    for (u64 granule = begin >> 16; granule <= (end - 1) >> 16; ++granule) {
        const auto it = dynamic_granules.find(granule);
        if (it == dynamic_granules.end()) {
            continue;
        }
        if (frame - it->second < DynamicFrames) {
            return true;
        }
        dynamic_granules.erase(it);
    }
    return false;
}

std::optional<std::pair<vk::Buffer, u64>> NativeRenderer::MirrorBuffer(VAddr address, u64 size) {
    static const bool enabled = [] {
        const char* env = std::getenv("RYGT_NATIVE_MIRRORS");
        return !(env && env[0] == '0');
    }();
    if (!enabled || size == 0 || size > 64_MB) {
        return std::nullopt;
    }
    SortRegionWrites();
    Mirror* mirror = MirrorHolding(address, size);
    if (mirror && !mirror->dirty_pages.empty() && !stream.list) {
        Begin(); // a new list makes the pending mirrors first (they can take this one in)
        mirror = MirrorHolding(address, size);
    }
    if (mirror) {
        RefreshMirror(*mirror);
        mirror->used_frame = frame;
        ++stats.mirror_binds;
        memo_kind = MemoKind::Mirror; // OPT-003
        memo_mirror = mirror;
        return std::pair{vk::Buffer{Handle<VkBuffer_T>(mirror->buffer->NativeHandle())},
                         u64{address - mirror->begin}};
    }
    // Not mirrored: a range bound in an earlier frame too gets a mirror at the start of the next
    // list (read in place until then).
    const VAddr begin = Common::AlignDown(address, 4096);
    // FIX-180: mirrors reach 256 KB past the range they are made for (RYGT_NATIVE_MIRROR_SLACK=
    // <KB> sets it, 0 as before). With mirrors cut at the range's end, Tokyo's world geometry
    // went missing for single frames in bursts, the sky colour showing through (Lance's
    // "vertex sky explosions"; 6-19 such frames a 450 s race, none with mirrors off). Mirror
    // contents matched guest memory on both the CPU (FIX-176) and the GPU (FIX-179) side and no
    // sampled draw read past its V#s (FIX-181), so the exact cause is still open; with the slack
    // the race had none (1 smaller jump in 13,000 frames), at the same speed (47-51 fps), with
    // about 105 bigger mirrors instead of 800.
    static const u64 slack = [] {
        const char* env = std::getenv("RYGT_NATIVE_MIRROR_SLACK");
        return (env && *env ? std::strtoull(env, nullptr, 10) : u64{256}) * 1024;
    }();
    const u64 reach = slack != 0 ? r.memory->ClampRangeSize(address, size + slack) : size;
    const VAddr end = Common::AlignUp(address + std::max(size, reach), 4096);
    if (mirror_candidates.size() > 65536) {
        mirror_candidates.clear(); // per-frame data at ever-new addresses
    }
    const auto [it, inserted] = mirror_candidates.try_emplace(begin, MirrorCandidate{end, frame});
    MirrorCandidate& candidate = it->second;
    if (inserted) {
        return std::nullopt;
    }
    candidate.end = std::max(candidate.end, end);
    if (candidate.frame == frame) {
        return std::nullopt;
    }
    candidate.frame = frame; // looked at again next frame at the earliest
    if (IsDynamic(begin, candidate.end)) {
        return std::nullopt;
    }
    pending_mirrors.emplace_back(begin, candidate.end);
    mirror_candidates.erase(it);
    return std::nullopt;
}

void NativeRenderer::RefreshMirror(Mirror& mirror) {
    if (mirror.dirty_pages.empty()) {
        return;
    }
    std::vector<VAddr> pages;
    pages.swap(mirror.dirty_pages);
    auto* direct = r.buffer_cache.GetDirectMemory();
    if (!direct) {
        return;
    }
    Stream& s = Begin();
    GlobalBarrier();
    const vk::Buffer dst{Handle<VkBuffer_T>(mirror.buffer->NativeHandle())};
    std::ranges::sort(pages);
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    for (const VAddr page : pages) {
        if (page < mirror.begin || page >= mirror.end) {
            continue;
        }
        direct->ForEachPart(page, std::min<u64>(4096, mirror.end - page),
                            [&](const VideoCore::Buffer& view, u64 view_offset, u64 offset,
                                u64 bytes) {
                                s.cmd.copyBuffer(view.Handle(), dst,
                                                 vk::BufferCopy{.srcOffset = view_offset,
                                                                .dstOffset = page - mirror.begin +
                                                                             offset,
                                                                .size = bytes});
                            });
        WatchPages(page, std::min<u64>(4096, mirror.end - page));
        const u64 g = (page - mirror.begin) / MirrorGranule; // FIX-176
        if (g < mirror.hashes.size()) {
            mirror.hashes[g] = 0;
        }
    }
    s.buffers_written = true;
    stats.mirror_refresh_pages += pages.size();
}

void NativeRenderer::MakePendingMirrors() {
    if (pending_mirrors.empty() || !stream.list) {
        return;
    }
    mirror_promotion_budget.BeginFrame(frame);
    if (!mirror_promotion_budget.Available()) {
        return; // Later draws still bind the original imported guest ranges.
    }
    std::vector<std::pair<VAddr, VAddr>> pending;
    pending.swap(pending_mirrors);
    std::ranges::sort(pending);
    std::vector<std::pair<VAddr, VAddr>> ranges; // overlapping or touching ranges joined
    for (const auto& [begin, end] : pending) {
        if (!ranges.empty() && begin <= ranges.back().second) {
            ranges.back().second = std::max(ranges.back().second, end);
        } else {
            ranges.emplace_back(begin, end);
        }
    }
    static const u64 budget = [] {
        const char* env = std::getenv("RYGT_NATIVE_MIRROR_MB");
        return (env ? std::strtoull(env, nullptr, 10) : 1024) * 1_MB;
    }();
    auto* direct = r.buffer_cache.GetDirectMemory();
    Stream& s = stream;
    bool copied = false;
    bool evicted = false;
    for (size_t range_index = 0; range_index < ranges.size(); ++range_index) {
        auto [begin, end] = ranges[range_index];
        // FIX-185 (diagnosis, RYGT_NATIVE_MIRROR_REGION_GAP=<KB>): no mirror within that distance
        // of a region.
        static const u64 region_gap = [] {
            const char* env = std::getenv("RYGT_NATIVE_MIRROR_REGION_GAP");
            return env && *env ? std::strtoull(env, nullptr, 10) * 1024 : u64{0};
        }();
        if (!direct || RegionOver(begin, end - begin) || IsDynamic(begin, end) ||
            (region_gap != 0 && RegionOver(begin > region_gap ? begin - region_gap : 0,
                                           end - begin + 2 * region_gap))) {
            continue; // shaders write there (the region holds it), or the CPU does
        }
        // The new mirror takes in the mirrors it overlaps.
        u64 taken = 0;
        for (auto it = FirstMirror(begin); it != mirrors.end() && it->second.begin < end; ++it) {
            begin = std::min(begin, it->second.begin);
            end = std::max(end, it->second.end);
            taken += it->second.end - it->second.begin;
        }
        if (end - begin > 256_MB) {
            continue;
        }
        if (!mirror_promotion_budget.TryStart(end - begin)) {
            // Preserve the original ranges, not pointers into mutable mirror
            // storage. Mapping, CPU/GPU writes and overlaps are checked again
            // when a later frame has budget. Nothing incomplete is published.
            pending_mirrors.insert(pending_mirrors.end(), ranges.begin() + range_index,
                                   ranges.end());
            break;
        }
        const auto promotion_start = std::chrono::steady_clock::now();
        struct AccountPromotion {
            OptionalMirrorBudget& budget;
            std::chrono::steady_clock::time_point start;
            ~AccountPromotion() { budget.Account(std::chrono::steady_clock::now() - start); }
        } account_promotion{mirror_promotion_budget, promotion_start};
        Native::SlowCallTrace<7> promotion_trace{true, "geometry-promotion",
            {"evict", "try-ready-copy", "unused", "retire-overlaps", "unused", "watch", "publish"}};
        promotion_trace.Next(0, begin, end - begin);
        if (mirror_bytes - taken + (end - begin) > budget && !evicted) {
            // Mirrors unused for two seconds go first.
            evicted = true;
            std::unique_lock map_lock{buffer_map_mutex}; // OPT-010
            for (auto it = mirrors.begin(); it != mirrors.end();) {
                if (it->second.used_frame + 120 < frame &&
                    !(it->second.begin < end && it->second.end > begin)) {
                    mirror_bytes -= it->second.end - it->second.begin;
                    ++stats.mirrors_evicted;
                    ++buffer_generation;
                    buffer_log.Note(it->second.begin, it->second.end);
                    retired_watch_ranges.emplace_back(it->second.begin, it->second.end);
                    it = mirrors.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (mirror_bytes - taken + (end - begin) > budget) {
            ++stats.mirror_over_budget;
            continue;
        }
        promotion_trace.Next(1);
        std::unique_ptr<Native::Rhi::Buffer> buffer;
        // Source views and destination must both be ready. Neither mutex may
        // make the recorder wait; a miss preserves the imported-buffer path.
        const bool ready = direct->TryForEachPart(begin, end - begin, [&] {
            buffer = mirror_preparation->TryAcquire(
                {begin, end, unmap_generation.load()});
            return bool(buffer);
        }, [&](const VideoCore::Buffer& view, u64 view_offset, u64 offset, u64 bytes) {
            const vk::Buffer dst{Handle<VkBuffer_T>(buffer->NativeHandle())};
            s.cmd.copyBuffer(view.Handle(), dst, vk::BufferCopy{
                .srcOffset = view_offset, .dstOffset = offset, .size = bytes});
        });
        if (!ready) {
            pending_mirrors.emplace_back(ranges[range_index]);
            continue;
        }
        promotion_trace.Next(3);
        ++buffer_generation; // Only actual map changes invalidate prepared draws.
        {
            std::unique_lock map_lock{buffer_map_mutex}; // OPT-010
            for (auto it = FirstMirror(begin); it != mirrors.end() && it->second.begin < end;) {
                mirror_bytes -= it->second.end - it->second.begin;
                retired_watch_ranges.emplace_back(it->second.begin, it->second.end);
                it = mirrors.erase(it); // freed once the GPU is done with it
            }
        }
        buffer_log.Note(begin, end); // OPT-010
        // Copy commands were recorded under the source-view guard above;
        // allocation and bounded import warming happened on the worker.
        // CPU writes to it from now on are copied in before its next use.
        promotion_trace.Next(5);
        WatchPages(begin, end - begin);
        mirror_bytes += end - begin;
        ++stats.mirrors_made;
        stats.mirror_made_bytes += end - begin;
        promotion_trace.Next(6);
        std::unique_lock map_lock{buffer_map_mutex}; // OPT-010
        mirrors.emplace(begin, Mirror{.begin = begin,
                                      .end = end,
                                      .buffer = std::move(buffer),
                                      .dirty_pages = {},
                                      .used_frame = frame});
        copied = true;
    }
    if (copied) {
        s.buffers_written = true; // the next draw waits for the copies
    }
}

void NativeRenderer::DropMirrors(VAddr address, u64 size) {
    for (auto it = FirstMirror(address); it != mirrors.end() && it->second.begin < address + size;) {
        mirror_bytes -= it->second.end - it->second.begin;
        ++buffer_generation; // OPT-003
        buffer_log.Note(it->second.begin, it->second.end); // OPT-010
        retired_watch_ranges.emplace_back(it->second.begin, it->second.end);
        std::unique_lock map_lock{buffer_map_mutex};
        it = mirrors.erase(it); // freed once the GPU is done with it
    }
}

void NativeRenderer::RefreshRegion(Region& region, VAddr address, u64 size) {
    SortRegionWrites();
    auto& dirty = region.dirty_pages;
    if (dirty.empty() || size == 0) {
        return;
    }
    if (!region.dirty_sorted) {
        std::ranges::sort(dirty);
        dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
        region.dirty_sorted = true;
    }
    // Regions survive scene changes and may hold unrelated allocations. Refresh only the
    // pages this operation uses; the rest stay pending, with their original CPU snapshots,
    // until a later operation uses them. GPU-written bytes still come from the region.
    const VAddr begin = Common::AlignDown(std::max(address, region.begin), 4096);
    const VAddr end = std::min(region.end, address + size);
    const auto first = std::lower_bound(dirty.begin(), dirty.end(), begin);
    const auto last = std::lower_bound(first, dirty.end(), end);
    if (first == last) {
        return;
    }
    std::vector<VAddr> pages(first, last);
    dirty.erase(first, last);
    const auto refresh_start = std::chrono::steady_clock::now();
    struct Timed {
        std::chrono::steady_clock::time_point start;
        u64& total;
        ~Timed() {
            total += std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
        }
    } timed{refresh_start, stats.region_refresh_ns};
    stats.region_refresh_pages += pages.size();
    auto* direct = r.buffer_cache.GetDirectMemory();
    if (!direct) {
        return;
    }
    Stream& s = Begin();
    GlobalBarrier();
    const vk::Buffer dst{Handle<VkBuffer_T>(region.buffer->NativeHandle())};
    // The selected dirty pages are already sorted and unique. Detach their old snapshots
    // before rearming protection: a write fault after that must save a new snapshot for the
    // next refresh, rather than have its snapshot consumed by this one.
    std::vector<std::unique_ptr<std::array<u8, 4096>>> snapshots(pages.size());
    {
        std::scoped_lock lock{region_dirty_mutex};
        for (size_t i = 0; i < pages.size(); ++i) {
            if (const auto it = page_snapshots.find(pages[i]); it != page_snapshots.end()) {
                snapshots[i] = std::move(it->second);
                page_snapshots.erase(it);
            }
        }
    }
    // One protection operation per contiguous run, instead of one per dirty page. All pages
    // are watched before comparing guest bytes, so writes racing with the comparison still
    // fault and remain pending for the next use.
    for (size_t first = 0; first < pages.size();) {
        size_t last = first + 1;
        while (last < pages.size() && pages[last] == pages[last - 1] + 4096) {
            ++last;
        }
        const VAddr end = std::min(region.end, pages[last - 1] + 4096);
        WatchPages(pages[first], end - pages[first]);
        first = last;
    }
    const auto copy_imported = [&](VAddr from, u64 size) {
        direct->ForEachPart(from, size,
                            [&](const VideoCore::Buffer& view, u64 view_offset, u64 offset,
                                u64 bytes) {
                                s.cmd.copyBuffer(view.Handle(), dst,
                                                 vk::BufferCopy{.srcOffset = view_offset,
                                                                .dstOffset = from - region.begin +
                                                                             offset,
                                                                .size = bytes});
                            });
    };
    const auto copy_snapshot = [&](VAddr from, const u8* data, u64 bytes) {
        // The CPU can rewrite/reuse the imported guest pages before a queued
        // copy executes. Upload exactly the bytes compared below from storage
        // retained by this stream until GPU completion, not from the live pages.
        const auto [buffer, offset, upload] = Allocate(bytes, 4);
        std::memcpy(upload, data, bytes);
        s.cmd.copyBuffer(buffer, dst,
                         vk::BufferCopy{.srcOffset = offset,
                                        .dstOffset = from - region.begin,
                                        .size = bytes});
    };
    for (size_t i = 0; i < pages.size(); ++i) {
        const VAddr page = pages[i];
        if (page < region.begin || page >= region.end) {
            continue;
        }
        const u64 page_bytes = std::min<u64>(4096, region.end - page);
        const auto& before = snapshots[i];
        std::array<u8, 4096> now;
        if (!Core::RytyMemory::GuardedCopy(now.data(), reinterpret_cast<const void*>(page),
                                         page_bytes)) {
            // GPU-only/unreadable memory must continue to use its GPU backing.
            copy_imported(page, page_bytes);
            continue;
        }
        if (!before) {
            copy_snapshot(page, now.data(), page_bytes);
            continue;
        }
        // The runs of 16-byte pieces the CPU changed (gaps under 64 bytes joined).
        constexpr u64 Piece = 16;
        u64 run_begin = 0, run_end = 0, changed = 0;
        bool in_run = false;
        for (u64 at = 0; at < page_bytes; at += Piece) {
            const u64 n = std::min(Piece, page_bytes - at);
            if (std::memcmp(now.data() + at, before->data() + at, n) == 0) {
                continue;
            }
            if (in_run && at - run_end < 64) {
                run_end = at + n;
                continue;
            }
            if (in_run) {
                copy_snapshot(page + run_begin, now.data() + run_begin, run_end - run_begin);
                changed += run_end - run_begin;
            }
            in_run = true;
            run_begin = at;
            run_end = at + n;
        }
        if (in_run) {
            copy_snapshot(page + run_begin, now.data() + run_begin, run_end - run_begin);
            changed += run_end - run_begin;
        }
        ++stats.region_partial_pages;
        stats.region_partial_bytes += changed;
    }
    s.buffers_written = true;
    ++stats.region_refreshes;
    if (stats.region_partial_pages != 0 && Sample(logged_partial)) {
        LOG_INFO(Render_Vulkan, "FIX-173: region pages copied in part: {} ({:.1f} KB of {:.1f} KB)",
                 stats.region_partial_pages, stats.region_partial_bytes / 1024.0,
                 stats.region_partial_pages * 4.0);
    }
}

u64 NativeRenderer::SnapshotBelow() {
    static const u64 below = [] {
        const char* env = std::getenv("RYGT_NATIVE_SNAPSHOT_BELOW");
        return env && *env ? std::strtoull(env, nullptr, 0) : u64{1_MB};
    }();
    // Race/replay buffers can depend on GPU writes not visible to a CPU snapshot.
    // The native session guard also covers attract replays using cached menu scripts.
    return RytyGtsMenuSnapshotsAllowed() ? below : 0;
}

std::optional<std::pair<vk::Buffer, u64>> NativeRenderer::GuestBuffer(VAddr address, u64 size,
                                                                      bool write, bool mirror) {
    if (size == 0) {
        return std::nullopt;
    }
    PhaseClock guest_clock{stats.guest_buffer_ns}; // NAT-145
    ++stats.guest_buffer_calls;
    last_mirror = nullptr; // OPT-004
    // OPT-003: the range's last result, when nothing it depends on changed.
    const u32 flags = u32(write) | (u32(mirror) << 1);
    const std::array<u64, 3> memo_key{address, size, flags};
    BufferMemo& memo =
        buffer_memos[XXH3_64bits(memo_key.data(), sizeof(memo_key)) & (buffer_memos.size() - 1)];
    if (memo.generation == buffer_generation && memo.address == address && memo.size == size &&
        memo.flags == flags) {
        if (memo.region || memo.mirror) {
            SortRegionWrites(); // CPU writes since: pages to copy in (may drop a mirror)
        }
        const bool fresh =
            memo.generation == buffer_generation &&
            (!memo.region || memo.region->dirty_pages.empty()) &&
            (!memo.mirror || memo.mirror->dirty_pages.empty());
        if (fresh) {
            if (memo.region) {
                ++stats.region_binds;
            } else if (memo.mirror) {
                ++stats.mirror_binds;
                memo.mirror->used_frame = frame;
            }
            last_bound = memo.bound;
            last_mirror = memo.mirror; // OPT-004
            ++stats.buffer_memo_hits;
            return std::pair{memo.buffer, memo.offset};
        }
    }
    memo_kind = MemoKind::None;
    memo_region = nullptr;
    memo_mirror = nullptr;
    const auto result = GuestBufferLookup(address, size, write, mirror);
    if (result && memo_kind == MemoKind::Mirror) {
        last_mirror = memo_mirror; // OPT-004
    }
    if (result && memo_kind != MemoKind::None) {
        memo = BufferMemo{
            .address = address,
            .size = size,
            .flags = flags,
            .generation = buffer_generation,
            .buffer = result->first,
            .offset = result->second,
            .region = memo_region,
            .mirror = memo_mirror,
            .bound = last_bound,
        };
    }
    return result;
}

std::optional<std::pair<vk::Buffer, u64>> NativeRenderer::GuestBufferLookup(VAddr address, u64 size,
                                                                            bool write,
                                                                            bool mirror) {
    last_bound = Bound::Region;
    // NAT-114: ranges in (or reaching into) a region use it.
    if (Region* region = RegionOver(address, size)) {
        if (address < region->begin || address + size > region->end) {
            region = MakeRegion(address, size);
        }
        if (region) {
            RefreshRegion(*region, address, size);
            ++stats.region_binds;
            memo_kind = MemoKind::Region; // OPT-003
            memo_region = region;
            return std::pair{vk::Buffer{Handle<VkBuffer_T>(region->buffer->NativeHandle())},
                             u64{address - region->begin}};
        }
    }
    // NAT-144: every range shaders write lives in video memory (a region), not in guest memory
    // over PCIe (the GPU was 85% busy waiting on PCIe, 55% with this; the race went from 25 to
    // 33-39 FPS). What the GPU writes there doesn't reach guest memory: GT Sport's CPU doesn't
    // read it back (the old renderer runs with buffer readbacks off too); small render targets
    // still go back (NAT-112). RYGT_NATIVE_WRITE_REGIONS=0: written ranges in guest memory.
    static const bool write_regions = [] {
        const char* env = std::getenv("RYGT_NATIVE_WRITE_REGIONS");
        return !(env && env[0] == '0');
    }();
    // FIX-150: GT Sport's small buffers from 0xf000000000 up are ones the GPU writes for the CPU
    // to read back (in a region the CPU never saw them: no rain drops on the windshield); written
    // ranges there stay in guest memory. RYGT_NATIVE_INPLACE_FROM=<hex address> moves the limit
    // (ffffffffffffffff: every written range gets a region, as before).
    static const VAddr inplace_from = [] {
        const char* env = std::getenv("RYGT_NATIVE_INPLACE_FROM");
        return env ? static_cast<VAddr>(std::strtoull(env, nullptr, 16)) : VAddr{0xf000000000};
    }();
    // FIX-172 (diagnosis, RYGT_NATIVE_INPLACE_RANGES=<hex address>+<hex size>,...): written
    // ranges meeting one of these stay in guest memory too.
    static const std::vector<std::pair<VAddr, VAddr>> inplace_ranges = [] {
        std::vector<std::pair<VAddr, VAddr>> list;
        const char* env = std::getenv("RYGT_NATIVE_INPLACE_RANGES");
        for (const char* at = env; at && *at;) {
            char* end = nullptr;
            const VAddr begin = std::strtoull(at, &end, 16);
            if (end == at || *end != '+') {
                break;
            }
            at = end + 1;
            const u64 length = std::strtoull(at, &end, 16);
            list.emplace_back(begin, begin + std::max<u64>(length, 1));
            at = *end == ',' ? end + 1 : end;
        }
        return list;
    }();
    const bool listed_in_place = std::ranges::any_of(inplace_ranges, [&](const auto& range) {
        return address < range.second && address + size > range.first;
    });
    if (write && write_regions && size <= 256_MB && address < inplace_from && !listed_in_place) {
        if (Region* region = MakeRegion(address, size)) {
            ++stats.region_binds;
            memo_kind = MemoKind::Region; // OPT-003
            memo_region = region;
            return std::pair{vk::Buffer{Handle<VkBuffer_T>(region->buffer->NativeHandle())},
                             u64{address - region->begin}};
        }
    }
    // FIX-170 (diagnosis, RYGT_NATIVE_SNAPSHOT_BELOW=<bytes>): small read-only shader buffers
    // (constants) are copied when the draw is recorded instead of read in place by the GPU later.
    if (!mirror && !write && size <= SnapshotBelow()) {
        last_bound = Bound::InPlace;
        return SnapshotRange(address, size);
    }
    if (mirror && !write) {
        if (const auto mirrored = MirrorBuffer(address, size)) {
            last_bound = Bound::Mirror; // NAT-150
            return mirrored;
        }
    }
    last_bound = Bound::InPlace;
    if (write) {
        DropMirrors(address, size); // NAT-150: the GPU writes guest memory there
    }
    if (auto* direct = r.buffer_cache.GetDirectMemory();
        direct && direct->EnsureImported(address, size)) {
        if (const auto found = direct->FindBuffer(address, size)) {
            if (!mirror) {
                memo_kind = MemoKind::InPlace; // OPT-003 (vertices and indices keep looking:
            }                                  // they become mirrors when bound again)
            return std::pair{found->first->Handle(), found->second};
        }
    }
    if (!write) {
        if (const auto gathered = GatherOnGpu(address, size)) {
            return gathered;
        }
    }
    if (write) {
        if (Region* region = MakeRegion(address, size)) {
            ++stats.region_binds;
            return std::pair{vk::Buffer{Handle<VkBuffer_T>(region->buffer->NativeHandle())},
                             u64{address - region->begin}};
        }
        ++stats.unbound;
        static std::atomic<u32> logged{};
        if (Sample(logged)) {
            LOG_WARNING(Render_Vulkan,
                        "NAT-101: written range {:#x}+{:#x} is not in one imported view; bound "
                        "empty",
                        address, size);
        }
        return std::nullopt;
    }
    if (size > 64_MB) {
        ++stats.unbound;
        static std::atomic<u32> logged{};
        if (Sample(logged)) {
            LOG_WARNING(Render_Vulkan,
                        "NAT-101: read range {:#x}+{:#x} is not in one imported view and too "
                        "large to copy; bound empty",
                        address, size);
        }
        return std::nullopt;
    }
    return SnapshotRange(address, size);
}

std::optional<std::pair<vk::Buffer, u64>> NativeRenderer::SnapshotRange(VAddr address, u64 size) {
    // A snapshot of what the range holds now, in the list's upload memory (zeros where the CPU
    // can't read it: NAT-106).
    const auto [buffer, offset, data] = Allocate(size, r.instance.StorageMinAlignment());
    const u64 readable = Native::ReadablePrefix(reinterpret_cast<const void*>(address), size);
    // FIX-160: the game can release part of the range meanwhile (zeros there then).
    if (readable != 0 &&
        !Core::RytyMemory::GuardedCopy(data, reinterpret_cast<const void*>(address), readable)) {
        r.memory->CopySparseMemory(address, data, readable);
    }
    if (readable < size) {
        std::memset(data + readable, 0, size - readable);
    }
    ++stats.copied_reads;
    stats.copied_bytes += size;
    return std::pair{buffer, offset};
}

std::optional<std::pair<vk::Buffer, u64>> NativeRenderer::GatherOnGpu(VAddr address, u64 size) {
    auto* direct = r.buffer_cache.GetDirectMemory();
    if (!direct || size > 256_MB || !direct->EnsureImported(address, size)) {
        return std::nullopt;
    }
    auto gathered = device->CreateBuffer({.size = size,
                                          .usage = Native::Rhi::BufferUsageStorage |
                                                   Native::Rhi::BufferUsageVertex |
                                                   Native::Rhi::BufferUsageIndex |
                                                   Native::Rhi::BufferUsageIndirect |
                                                   Native::Rhi::BufferUsageTransferDst,
                                          .memory = Native::Rhi::MemoryKind::DeviceLocal,
                                          .name = "native gather"});
    if (!gathered) {
        return std::nullopt;
    }
    Stream& s = Begin();
    // Copies run outside rendering, after what earlier work wrote; the draw waits for them
    // (the next barrier: they count as written buffers).
    GlobalBarrier();
    const vk::Buffer dst{Handle<VkBuffer_T>(gathered->NativeHandle())};
    const bool all = direct->ForEachPart(
        address, size, [&](const VideoCore::Buffer& view, u64 view_offset, u64 offset, u64 bytes) {
            s.cmd.copyBuffer(view.Handle(), dst,
                             vk::BufferCopy{.srcOffset = view_offset, .dstOffset = offset,
                                            .size = bytes});
        });
    s.buffers_written = true;
    s.keep.push_back(std::move(gathered));
    if (!all) {
        return std::nullopt;
    }
    ++stats.gathers;
    stats.gathered_bytes += size;
    return std::pair{dst, u64{0}};
}

void NativeRenderer::ResolveUiDisplay(VAddr address, bool keep) {
    auto& state = UiHires::Get();
    std::scoped_lock lock{state.mutex};
    const auto found = state.proxies.find(address);
    if (found == state.proxies.end() || found->second.phase != UiHires::Phase::Ui) {
        return;
    }
    auto& proxy = found->second;
    if (!proxy.native_stale) {
        if (!keep) {
            proxy.phase = UiHires::Phase::Off;
        }
        return;
    }
    auto* entry = registry->FindExisting(
        address,
        [&](const Native::ImageEntry& candidate) {
            return !candidate.from_cpu && !candidate.info.props.is_depth &&
                   candidate.info.num_samples == 1 && candidate.info.resources.levels == 1 &&
                   candidate.info.resources.layers == 1 &&
                   candidate.info.size.width >= proxy.native.width &&
                   candidate.info.size.height >= proxy.native.height;
        },
        true);
    if (!entry) {
        return;
    }
    const bool changed = proxy.native_stale;
    Splice();
    r.scheduler.EndRendering();
    UiHires::ResolveNative(r.scheduler.DirectCommandBuffer(),
                           vk::Image{Handle<VkImage_T>(entry->image->NativeHandle())}, proxy, keep);
    if (changed) {
        Written(entry);
        if (++ui_resolves == 1) {
            LOG_INFO(
                Render_Vulkan,
                "Native UI: synchronized output-resolution UI to a guest display read at {:#x}",
                address);
        }
    }
    entry->last_use.store(frame, std::memory_order_relaxed);
}

void NativeRenderer::SyncUiReads(std::span<const Shader::Info* const> stages) {
    if (!UiHires::Enabled()) {
        return;
    }
    const bool dirty = UiHires::Get().stale.load(std::memory_order_relaxed) != 0;
    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        for (const auto& image : stage->images) {
            if (!dirty && !image.is_written) {
                continue;
            }
            const auto sharp = image.GetSharp(*stage);
            if (UiHires::KnownAddress(sharp.Address())) {
                ResolveUiDisplay(sharp.Address(), !image.is_written);
            }
        }
    }
}

void NativeRenderer::SyncUiRange(VAddr address, u64 size, bool keep) {
    if (!UiHires::Enabled() || size == 0) {
        return;
    }
    std::vector<VAddr> overlaps;
    auto& state = UiHires::Get();
    {
        std::scoped_lock lock{state.mutex};
        for (const auto& [base, proxy] : state.proxies) {
            if (proxy.phase != UiHires::Phase::Ui) {
                continue;
            }
            const auto* entry = registry->FindExisting(base, [](const Native::ImageEntry& image) {
                return !image.from_cpu && !image.info.props.is_depth;
            });
            const u64 bytes =
                entry ? entry->info.guest_size : u64(proxy.native.width) * proxy.native.height * 4;
            if ((base >= address && base - address < size) ||
                (address >= base && address - base < bytes)) {
                overlaps.push_back(base);
            }
        }
    }
    for (const auto base : overlaps) {
        ResolveUiDisplay(base, keep);
    }
}

bool NativeRenderer::PrepareUiTarget(Targets& targets, const GraphicsPipeline& pipeline) {
    auto& state = UiHires::Get();
    state.scale = 0.f;
    const bool ui_layer = pipeline.GetGraphicsKey().color_buffers[0].hdr_ui_layer;
    if (!UiHires::Enabled()) {
        return !ui_layer;
    }
    std::unique_lock lock{state.mutex};
    for (u32 cb = 0; cb < targets.state.num_color_attachments; ++cb) {
        const auto* entry = targets.colors[cb];
        if (!entry) {
            continue;
        }
        const auto found = state.proxies.find(entry->info.guest_address);
        if (found == state.proxies.end() || found->second.size.width == 0) {
            continue;
        }
        auto& proxy = found->second;
        const bool supported = targets.state.num_color_attachments == 1 && !targets.depth &&
                               entry->info.num_samples == 1 && entry->info.resources.layers == 1 &&
                               entry->info.resources.levels == 1 &&
                               entry->info.size.width >= proxy.native.width &&
                               entry->info.size.height >= proxy.native.height &&
                               targets.state.width >= proxy.native.width &&
                               targets.state.height >= proxy.native.height;
        if (proxy.phase == UiHires::Phase::Off) {
            return !ui_layer;
        }
        if (!supported) {
            if (proxy.phase == UiHires::Phase::Ui) {
                lock.unlock();
                ResolveUiDisplay(entry->info.guest_address, false);
            } else {
                proxy.phase = UiHires::Phase::Off;
            }
            return !ui_layer;
        }
        if (!ui_layer && proxy.phase == UiHires::Phase::Idle) {
            proxy.phase = UiHires::Phase::Scene;
            return true;
        }
        if (proxy.phase != UiHires::Phase::Ui) {
            if (!UiHires::CreateImages(r.instance, proxy, targets.color_formats[cb])) {
                proxy.phase = UiHires::Phase::Off;
                return !ui_layer;
            }
            if (proxy.phase == UiHires::Phase::Idle) {
                Splice();
                r.scheduler.EndRendering();
                UiHires::StartWithoutScene(r.scheduler, proxy);
            } else {
                auto* source = CropDisplay(*targets.colors[cb], proxy.native);
                if (!source) {
                    proxy.phase = UiHires::Phase::Off;
                    return !ui_layer;
                }
                Splice();
                r.scheduler.EndRendering();
                VideoCore::ImageViewInfo view_info{};
                view_info.format = targets.color_formats[cb];
                const auto view = View(*source, view_info, ViewUse::Sampled);
                if (!view) {
                    proxy.phase = UiHires::Phase::Off;
                    return !ui_layer;
                }
                UiHires::UpscaleNative(r.scheduler.DirectCommandBuffer(),
                                       vk::Image{Handle<VkImage_T>(source->image->NativeHandle())},
                                       view, view_info.format, proxy);
            }
        }
        auto& attachment = targets.state.color_attachments[cb];
        attachment.image_view =
            ui_layer ? *proxy.ui_view
                     : UiHires::View(r.instance, proxy, targets.color_formats[cb], false);
        attachment.image_layout = vk::ImageLayout::eGeneral;
        targets.state.width = proxy.size.width;
        targets.state.height = proxy.size.height;
        proxy.ui_used |= ui_layer;
        UiHires::MarkStale(proxy, true);
        state.scale = float(proxy.size.width) / proxy.native.width;
        if (++ui_draws == 1) {
            LOG_INFO(Render_Vulkan,
                     "Native UI: drawing {}x{} over a {}x{} scene; DLSS {}, HDR layer {}",
                     proxy.size.width, proxy.size.height, proxy.native.width, proxy.native.height,
                     proxy.dlss_used, ui_layer);
        }
        return true;
    }
    return !ui_layer;
}

bool NativeRenderer::ResolveTargets(const AmdGpu::Regs& regs, const GraphicsPipeline& pipeline,
                                    Targets& out) {
    const auto& key = pipeline.GetGraphicsKey();
    RenderState& state = out.state;
    state = {};
    out.colors = {};
    out.depth = nullptr;
    u32 width = r.instance.GetMaxFramebufferWidth();
    u32 height = r.instance.GetMaxFramebufferHeight();
    u32 num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = static_cast<u16>(std::bit_width(key.mrt_mask));
    const bool skip_cb = regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
        auto& attachment = state.color_attachments[cb];
        attachment = {};
        attachment.image_layout = vk::ImageLayout::eGeneral;
        const auto& col_buf = regs.color_buffers[cb];
        if (skip_cb || !col_buf || !regs.color_target_mask.GetMask(cb) ||
            (key.mrt_mask & (1u << cb)) == 0) {
            continue;
        }
        const VideoCore::TextureCache::ImageDesc desc{col_buf, r.liverpool->DrawCbExtent(cb)};
        Native::ImageEntry* entry = registry->Find(desc.info, true, frame);
        if (!entry) {
            ++stats.null_images;
            continue;
        }
        const vk::ImageView view = View(*entry, desc.view_info, ViewUse::Attachment);
        if (!view) {
            continue;
        }
        ApplyRewrite(*entry); // NAT-120
        if (Native::WatchedTexture(entry->info.guest_address, entry->info.guest_address + 1) &&
            Native::WatchSample()) {
            LOG_WARNING(Render_Vulkan, "FIX-153 watch: draw into target {:#x} {}x{} layers {} "
                        "({:#x} bytes), view layers {}+{} mip {}",
                        entry->info.guest_address, entry->info.size.width,
                        entry->info.size.height, entry->info.resources.layers,
                        entry->info.guest_size, desc.view_info.range.base.layer,
                        desc.view_info.range.extent.layers, desc.view_info.range.base.level);
        }
        out.colors[cb] = entry;
        out.color_formats[cb] = desc.view_info.format;
        // FIX-154: the draw covers the target it names, not the larger picture it may share
        // (a clear of the small one must leave the rest alone).
        const u32 mip = desc.view_info.range.base.level;
        width = std::min<u32>(width, std::max(std::min(entry->info.size.width,
                                                       desc.info.size.width) >> mip, 1u));
        height = std::min<u32>(height, std::max(std::min(entry->info.size.height,
                                                         desc.info.size.height) >> mip, 1u));
        num_layers = std::min<u32>(num_layers, desc.view_info.range.extent.layers);
        const VAddr cmask = col_buf.CmaskAddress();
        bool is_clear = false;
        if (cmask != 0) {
            known_meta.insert(cmask);
            is_clear = cleared_meta.erase(cmask) != 0;
        }
        attachment.image_view = view;
        attachment.clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf).color.uint32
                     : std::array<u32, 4>{};
        attachment.is_clear = is_clear;
    }
    auto& db = state.depth_stencil_attachment;
    db = {};
    db.image_layout = vk::ImageLayout::eGeneral;
    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const VAddr htile = regs.depth_htile_data_base.GetAddress();
        const VideoCore::TextureCache::ImageDesc desc{regs.depth_buffer, regs.depth_view,
                                                      regs.depth_control, htile,
                                                      r.liverpool->DrawDbExtent()};
        Native::ImageEntry* entry = registry->Find(desc.info, true, frame);
        const vk::ImageView view =
            entry ? View(*entry, desc.view_info, ViewUse::Attachment) : vk::ImageView{};
        if (view) {
            ApplyRewrite(*entry); // NAT-120
            if (regs.depth_buffer.StencilValid() && entry->info.props.has_stencil) {
                auto& owners = stencil_owners[regs.depth_buffer.StencilAddress()];
                if (std::ranges::find(owners, entry) == owners.end()) {
                    owners.push_back(entry);
                }
            }
            bool meta_clear = false;
            bool fresh = false;
            if (htile != 0) {
                known_meta.insert(htile);
                meta_clear = cleared_meta.erase(htile) != 0;
                // FIX-156: a depth buffer with HTILE the GPU never drew into starts out cleared,
                // as in the old renderer (a new depth image's HTILE counts as cleared). The game
                // fast-clears the HTILE before the first pass that uses the buffer, while that
                // memory is not yet known as metadata. GT Sport draws the track from above once
                // while a race loads, and the cockpit's night lighting reads a map made of that
                // depth: started from the zeros of fresh memory, every fragment failed the depth
                // test, the map came out empty, and the cockpit went dark but for the odd frame.
                if (!meta_clear && desc.info.meta_info.htile_addr != 0 && !entry->from_cpu &&
                    entry->write_version.load(std::memory_order_relaxed) == 0) {
                    meta_clear = fresh = true;
                    ++stats.meta_clears;
                }
            }
            const auto aspects =
                DepthStencilClearAspects(regs.depth_buffer, regs.depth_control,
                                         regs.depth_render_control, meta_clear);
            const bool depth_clear = bool(aspects & vk::ImageAspectFlagBits::eDepth);
            const bool stencil_clear = bool(aspects & vk::ImageAspectFlagBits::eStencil);
            if (Native::WatchedTexture(entry->info.guest_address, entry->info.guest_address + 1) &&
                Native::WatchSample()) {
                const auto& dc = regs.depth_control;
                LOG_WARNING(Render_Vulkan, "FIX-156 watch: depth pass {:#x} {}x{} htile {:#x} "
                            "(tile surface {}) version {} meta clear {}{} -> depth clear {} ({}) "
                            "stencil clear {} | test {} write {} func {} stencil {} | render "
                            "control clear {}/{} (frame {})",
                            entry->info.guest_address, entry->info.size.width,
                            entry->info.size.height, htile,
                            u32(regs.depth_buffer.z_info.tile_surface_enable),
                            entry->write_version.load(), meta_clear, fresh ? " (fresh)" : "",
                            depth_clear, regs.depth_clear, stencil_clear, u32(dc.depth_enable),
                            u32(dc.depth_write_enable), u32(dc.depth_func),
                            u32(dc.stencil_enable),
                            u32(regs.depth_render_control.depth_clear_enable),
                            u32(regs.depth_render_control.stencil_clear_enable), frame);
            }
            db.image_view = view;
            if (r.FrameDumpCollecting() && entry->info.guest_address == 0x100ca70000) {
                r.FrameDumpEvent(fmt::format(
                    "shadow binding: target {}x{} layers {}, view layer {}+{}, instances {}, "
                    "indices {}, depth clear {}, stencil clear {}",
                    entry->info.size.width, entry->info.size.height, entry->info.resources.layers,
                    desc.view_info.range.base.layer, desc.view_info.range.extent.layers,
                    regs.num_instances.NumInstances(), regs.num_indices, depth_clear, stencil_clear));
            }
            if (regs.depth_buffer.DepthValid()) {
                db.clear_value[0] = depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
                db.has_depth = true;
                db.depth_clear = depth_clear;
            }
            if (regs.depth_buffer.StencilValid() && entry->info.props.has_stencil) {
                db.clear_value[1] = stencil_clear ? regs.stencil_clear : 0u;
                db.has_stencil = true;
                db.stencil_clear = stencil_clear;
            }
            width = std::min<u32>(width, std::min(entry->info.size.width, desc.info.size.width));
            height =
                std::min<u32>(height, std::min(entry->info.size.height, desc.info.size.height));
            num_layers = std::min<u32>(num_layers, desc.view_info.range.extent.layers);
            out.depth = entry;
        } else {
            ++stats.null_images;
        }
    }
    state.width = static_cast<u16>(std::min<u32>(width, 0xffff));
    state.height = static_cast<u16>(std::min<u32>(height, 0xffff));
    state.num_layers = static_cast<u16>(
        num_layers == std::numeric_limits<u16>::max() ? 1u : std::max(num_layers, 1u));
    return true;
}

void NativeRenderer::BindStages(std::span<const Shader::Info* const> stages,
                                const AmdGpu::Regs& regs, bool compute, DrawBindings& b) {
    size_t total = 0;
    for (const auto* stage : stages) {
        if (stage) {
            total += stage->buffers.size() + stage->images.size() + stage->samplers.size();
        }
    }
    b.writes.reserve(total);
    Shader::Backend::Bindings binding{};
    std::array<StageResourceBindings, Shader::MaxStageTypes> plans{};
    for (u32 i = 0; i < stages.size(); ++i) {
        const auto* stage = stages[i];
        if (!stage) {
            continue;
        }
        // SHD-002: each stage binds in its own set, from its own start.
        plans[i] = PlanStageResourceBindings(*stage,
                                             Shader::Backend::StageStart(u32(stage->sw_stage)));
        auto buffer_binding = plans[i].buffers;
        PhaseClock clock{stats.phase_ns[2]};
        BindBuffers(*stage, regs, compute, buffer_binding, b);
        binding = plans[i].next;
    }
    for (u32 i = 0; i < stages.size(); ++i) {
        if (const auto* stage = stages[i]) {
            auto texture_binding = plans[i].textures;
            PhaseClock clock{stats.phase_ns[3]};
            BindTextures(*stage, regs, texture_binding, b);
        }
    }
}

void NativeRenderer::BindBuffers(const Shader::Info& stage, const AmdGpu::Regs& regs, bool compute,
                                 Shader::Backend::Bindings& binding, DrawBindings& b) {
    const u64 alignment = r.instance.StorageMinAlignment();
    if (r.FrameDumpCollecting() && !compute &&
        regs.depth_buffer.DepthAddress() == 0x100ca70000 &&
        stage.sw_stage == Shader::SwStage::Vertex) {
        std::string values;
        for (size_t i = 0; i < std::min<size_t>(stage.flattened_ud_buf.size(), 64); ++i) {
            values += fmt::format(" {}:{:08x}@{:x}", i, stage.flattened_ud_buf[i],
                i < stage.flattened_ud_src.size() ? stage.flattened_ud_src[i] : 0);
        }
        r.FrameDumpEvent(fmt::format("shadow constants vs {:#x}:{}", stage.pgm_hash, values));
    }
    // DIAG-150 (SHADGT_WATCH_SHADERS=<hex,...>): a watched shader's buffers, how they are bound
    // and the first bytes guest memory holds there, and its flattened user data.
    if (Rasterizer::ExposureLogOn()) {
        r.ExposureLogShader(stage, "native"); // FIX-153 (diagnosis)
    }
    static const bool watching = std::getenv("SHADGT_WATCH_SHADERS") != nullptr;
    const bool watched = watching && IsWatchedShader(stage.pgm_hash);
    static std::atomic<u64> watched_binds{};
    const u64 watched_bind = watched ? ++watched_binds : 0;
    const bool log_watched = watched && (watched_bind <= 8 || watched_bind % 300 == 0);
    if (log_watched) {
        std::string flat;
        for (size_t i = 0; i < std::min<size_t>(stage.flattened_ud_buf.size(), 192); ++i) {
            flat += fmt::format(" {:08x}", stage.flattened_ud_buf[i]);
        }
        LOG_WARNING(Render_Vulkan, "DIAG-150: native shader {:#x} use {}: {} dwords flattened:{}",
                    stage.pgm_hash, watched_bind, stage.flattened_ud_buf.size(), flat);
        // FIX-153 (diagnosis): where the walker read each run of flattened dwords.
        std::string sources;
        const auto& src = stage.flattened_ud_src;
        for (size_t i = 0; i < src.size();) {
            size_t j = i + 1;
            while (j < src.size() && src[j] != 0 && src[j] == src[j - 1] + 4) {
                ++j;
            }
            if (src[i] != 0) {
                sources += fmt::format(" [{}..{}] {:#x}", i, j - 1, src[i]);
            }
            i = j;
        }
        LOG_WARNING(Render_Vulkan, "DIAG-150: native shader {:#x} use {} sources:{}",
                    stage.pgm_hash, watched_bind, sources);
    }
    // AMD defaults to the validated descriptor-backed buffer path
    // rather than repeating guest page-table walks for each scalar constant load.
    static const bool composition_descriptors = [&] {
        const char* env = std::getenv("RYGT_COMPOSITION_BUFFER_DESCRIPTORS");
        return env && *env ? env[0] == '1' : r.instance.GetVendorID() == 0x1002;
    }();
    const bool descriptor_composition = composition_descriptors &&
        stage.sw_stage == Shader::SwStage::Fragment && IsSceneCompositionShader(stage.pgm_hash);
    static const bool postprocess_descriptors = [&] {
        const char* env = std::getenv("RYGT_POSTPROCESS_BUFFER_DESCRIPTORS");
        return env && *env ? env[0] == '1' : r.instance.GetVendorID() == 0x1002;
    }();
    const bool descriptor_postprocess = postprocess_descriptors &&
        stage.sw_stage == Shader::SwStage::Fragment &&
        (stage.pgm_hash == 0x184b619c || stage.pgm_hash == 0x7cd92fb8);
    // Diagnostic A/B: retain shader compilation but use descriptor bindings.
    static const bool descriptor_only_a82d2fe = [] {
        const char* value = std::getenv("RYGT_A82D2FE_DESCRIPTOR_ONLY");
        return value && value[0] == '1';
    }();
    const bool force_descriptor = descriptor_only_a82d2fe &&
        stage.sw_stage == Shader::SwStage::Compute && stage.pgm_hash == 0xa82d2fe;
    static std::atomic<bool> descriptor_diagnostic_logged{};
    if (force_descriptor && !descriptor_diagnostic_logged.exchange(true))
        LOG_WARNING(Render_Vulkan, "DIAG: 0xa82d2fe descriptor-only buffer binding active");
    auto* direct = stage.uses_direct && !descriptor_composition && !force_descriptor
        ? r.buffer_cache.GetDirectMemory() : nullptr;
    // D3's table: page table L1 and zero page, then base, size and valid of each buffer read
    // through it.
    static thread_local std::vector<std::array<u32, 4>> direct_entries;
    direct_entries.assign(stage.buffers.size(), {});
    constexpr u64 GpuAddressLimit = 1ULL << 40;
    u32 index = 0;
    for (const auto& desc : stage.buffers) {
        const u32 buffer_index = index++;
        vk::DescriptorBufferInfo info{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
        if (desc.IsSpecial()) {
            switch (desc.buffer_type) {
            case Shader::BufferType::GdsBuffer:
                info = {Handle<VkBuffer_T>(gds->NativeHandle()), 0, gds->Size()};
                break;
            case Shader::BufferType::Flatbuf: {
                const u32 size = static_cast<u32>(stage.flattened_ud_buf.size() * sizeof(u32));
                if (size != 0) {
                    const auto [buffer, offset, data] = Allocate(size, alignment);
                    std::memcpy(data, stage.flattened_ud_buf.data(), size);
                    info = {buffer, offset, size};
                }
                break;
            }
            case Shader::BufferType::ClipPlanes:
                if (regs.clipper_control.user_clip_plane_enable != 0) {
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = regs.clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const auto [buffer, offset, data] = Allocate(sizeof(planes), alignment);
                    std::memcpy(data, planes.data(), sizeof(planes));
                    info = {buffer, offset, sizeof(planes)};
                }
                break;
            case Shader::BufferType::DirectTable: {
                const u32 words = static_cast<u32>(4 + stage.buffers.size() * 4);
                const auto [buffer, offset, data] = Allocate(words * sizeof(u32), alignment);
                auto* table = reinterpret_cast<u32*>(data);
                std::memset(table, 0, words * sizeof(u32));
                if (auto* memory = r.buffer_cache.GetDirectMemory()) {
                    const u64 l1 = memory->L1Address();
                    const u64 zero = memory->ZeroPageAddress();
                    table[0] = u32(l1);
                    table[1] = u32(l1 >> 32);
                    table[2] = u32(zero);
                    table[3] = u32(zero >> 32);
                    for (size_t i = 0; i < stage.buffers.size(); ++i) {
                        std::memcpy(&table[4 + i * 4], direct_entries[i].data(), 16);
                    }
                }
                info = {buffer, offset, words * sizeof(u32)};
                break;
            }
            case Shader::BufferType::SharedMemory: {
                const auto& cs_program = r.liverpool->DrawCsRegs();
                const u64 lds_size =
                    std::max<u64>(u64(cs_program.SharedMemSize()) * cs_program.NumWorkgroups(), 16);
                // GCN LDS is undefined when a workgroup starts: one GPU-only buffer serves every
                // dispatch (they are separated by barriers).
                if (!lds || lds->Size() < lds_size) {
                    lds = device->CreateBuffer({
                        .size = std::max<u64>(Common::AlignUp(lds_size, 1_MB), 4_MB),
                        .usage = Native::Rhi::BufferUsageStorage,
                        .memory = Native::Rhi::MemoryKind::DeviceLocal,
                        .name = "native lds",
                    });
                }
                if (lds) {
                    info = {Handle<VkBuffer_T>(lds->NativeHandle()), 0, lds_size};
                }
                break;
            }
            default: {
                static std::atomic<u32> logged{};
                if (Sample(logged)) {
                    LOG_WARNING(Render_Vulkan,
                                "NAT-101: shader {:#x} uses buffer type {}, not supported in "
                                "native mode; bound empty",
                                stage.pgm_hash, u32(desc.buffer_type));
                }
                break;
            }
            }
        } else {
            const auto vsharp = desc.GetSharp(stage);
            const u64 base = vsharp.base_address;
            const u64 sharp_size = vsharp.GetSize();
            // DIAG-204 (RYGT_PROBE_SHADER=<hash>): that program's buffers and their first bytes.
            static const u64 probe_hash = [] {
                const char* env = std::getenv("RYGT_PROBE_SHADER");
                return env && *env ? std::strtoull(env, nullptr, 16) : 0;
            }();
            if (probe_hash != 0 && stage.pgm_hash == probe_hash) {
                static std::atomic<u32> probes{};
                if (const u32 n = probes++; n < 64) {
                    std::array<u32, 16> head{};
                    const bool read = base != 0 && Core::RytyMemory::GuardedCopy(
                        head.data(), reinterpret_cast<const void*>(base), sizeof(head));
                    LOG_WARNING(Render_Vulkan,
                                "DIAG-204: {:#x} buffer {} V# base {:#x} stride {} records {} "
                                "dfmt {} nfmt {} swizzle {} tid {}: {}",
                                stage.pgm_hash, buffer_index, base, vsharp.GetStride(),
                                vsharp.num_records, u32(vsharp.GetDataFmt()),
                                u32(vsharp.GetNumberFmt()), u32(vsharp.swizzle_enable),
                                u32(vsharp.add_tid_enable),
                                read ? fmt::format("{:08x}", fmt::join(head, " "))
                                     : std::string{"unreadable"});
                }
            }
            // FIX-031 / FIX-036 / FIX-046: V#s that are not buffers are bound empty.
            const bool impossible =
                vsharp.num_records != UINT32_MAX &&
                (sharp_size >= GpuAddressLimit || base + sharp_size > GpuAddressLimit ||
                 (sharp_size >= 1_GB && base != 0 && !r.memory->IsValidMapping(base)));
            const bool unmapped =
                !impossible && base != 0 && sharp_size != 0 && !r.IsMappedStart(base);
            if (base != 0 && sharp_size != 0 && !impossible && !unmapped) {
                const u64 size = r.memory->ClampRangeSize(base, sharp_size);
                // FIX-174 (diagnosis, RYGT_NATIVE_WATCH_BUFFER=<hex address>+<hex size>): the
                // shaders that bind a buffer over that range (the first ones, then now and then).
                static const std::pair<VAddr, VAddr> watch_buffer = [] {
                    const char* env = std::getenv("RYGT_NATIVE_WATCH_BUFFER");
                    if (!env || !*env) {
                        return std::pair<VAddr, VAddr>{};
                    }
                    char* end = nullptr;
                    const VAddr at = std::strtoull(env, &end, 16);
                    const u64 length = end && *end == '+' ? std::strtoull(end + 1, nullptr, 16) : 1;
                    return std::pair<VAddr, VAddr>{at, at + std::max<u64>(length, 1)};
                }();
                if (watch_buffer.first != 0 && base < watch_buffer.second &&
                    base + size > watch_buffer.first) {
                    static std::atomic<u32> watch_logged{};
                    if (Sample(watch_logged)) {
                        LOG_WARNING(Render_Vulkan,
                                    "FIX-174 watch: {} shader {:#x} binds buffer {} {:#x}+{:#x} "
                                    "(frame {})",
                                    stage.sw_stage == Shader::SwStage::Compute ? "compute" : "draw",
                                    stage.pgm_hash, desc.is_written ? "write" : "read", base,
                                    size, frame);
                    }
                }
                const auto log_buffer = [&](const char* how) {
                    if (!log_watched) {
                        return;
                    }
                    std::array<u32, 4> head{};
                    const bool read = Core::RytyMemory::GuardedCopy(head.data(),
                                                        reinterpret_cast<const void*>(base),
                                                        std::min<u64>(size, sizeof(head)));
                    LOG_WARNING(Render_Vulkan,
                                "DIAG-150: native shader {:#x} use {} buffer {} {} {:#x}+{:#x} "
                                "stride {} records {:#x}: {}; memory {}",
                                stage.pgm_hash, watched_bind, buffer_index,
                                desc.is_written ? "write" : "read", base, size, vsharp.GetStride(),
                                vsharp.num_records, how,
                                read ? fmt::format("{:08x} {:08x} {:08x} {:08x}", head[0], head[1],
                                                   head[2], head[3])
                                     : std::string{"unreadable"});
                };
                // This known shader's 12-byte arguments are CPU command parameters. Freeze
                // them once for both the shader fallback and transfer path; GPU-backed
                // arguments retain the normal shader path and are never read as CPU values.
                const bool freeze_fill_args = stage.pgm_hash == 0x8aa6e00c &&
                    !desc.is_written && size >= 12 &&
                    (size == 12 || size == 16 ||
                     desc.post_op == Shader::SharpFetchPostOp::ReadConstPointer) &&
                    !RegionOver(base, 12);
                if (!freeze_fill_args && direct &&
                    !(descriptor_postprocess && !desc.is_written) &&
                    desc.IsDirectEligible() && size < (u64{1} << 32) &&
                    (desc.is_written || size > SnapshotBelow()) && // FIX-170
                    !RegionOver(base, size) && direct->EnsureImported(base, size)) {
                    // D3: read in place through the page table; nothing bound.
                    direct_entries[buffer_index] = {u32(base), u32(base >> 32), u32(size), 1u};
                    if (SnapshotBelow() >= 4_KB && stage.hw_stage == Shader::HwStage::Compute &&
                        size >= 4_KB &&
                        desc.post_op == Shader::SharpFetchPostOp::ReadConstPointer) {
                        constexpr u64 PrefixSize = 4_KB;
                        const auto [source, offset] = SnapshotRange(base, PrefixSize).value();
                        auto prefix = device->CreateBuffer({
                            .size = PrefixSize,
                            .usage = Native::Rhi::BufferUsageStorage |
                                     Native::Rhi::BufferUsageTransferDst,
                            .memory = Native::Rhi::MemoryKind::DeviceLocal,
                            .name = "native scalar prefix"});
                        if (prefix) {
                            EndRendering();
                            const vk::Buffer target{Handle<VkBuffer_T>(prefix->NativeHandle())};
                            stream.cmd.copyBuffer(source, target,
                                vk::BufferCopy{.srcOffset = offset, .dstOffset = 0,
                                               .size = PrefixSize});
                            stream.list->Barrier(Native::Rhi::ScopeTransfer,
                                                 Native::Rhi::ScopeCompute);
                            info = {target, 0, PrefixSize};
                            direct_entries[buffer_index][3] = u32(PrefixSize);
                            stream.keep.push_back(std::move(prefix));
                        }
                    }
                    // Bounded zero-copy scalar prefix: alias an existing imported view.
                    // No gather, snapshot or new buffer allocation; out-of-prefix reads
                    // retain the existing direct translation and its full bounds.
                    static const bool prefix_descriptor = [&] {
                        const char* env = std::getenv("RYGT_COMPUTE_PREFIX_DESCRIPTOR");
                        return env && *env ? env[0] == '1' : r.instance.GetVendorID() == 0x1002;
                    }();
                    if (prefix_descriptor && stage.sw_stage == Shader::SwStage::Compute &&
                        stage.pgm_hash == 0xd0af5a54 && !desc.is_written && size >= 4_KB &&
                        direct_entries[buffer_index][3] == 1u &&
                        desc.post_op == Shader::SharpFetchPostOp::ReadConstPointer) {
                        constexpr u64 PrefixSize = 4_KB;
                        if (const auto found = direct->FindBuffer(base, PrefixSize)) {
                            const u64 aligned = Common::AlignDown(found->second, alignment);
                            const u64 adjust = found->second - aligned;
                            b.push.AddOffset(binding.buffer, static_cast<u32>(adjust));
                            info = {found->first->Handle(), aligned, PrefixSize + adjust};
                            direct_entries[buffer_index][3] = u32(PrefixSize);
                        }
                    }
                    if (InPlaceSampled()) {
                        in_place.Note(2, base, size); // NAT-142
                    }
                    log_buffer("in place through the page table (D3)");
                } else if (const auto found = freeze_fill_args
                                                  ? SnapshotRange(base, 12)
                                                  : GuestBuffer(base, size, desc.is_written)) {
                    log_buffer(last_bound == Bound::Region   ? "region"
                               : last_bound == Bound::Mirror ? "mirror"
                                                             : "in place");
                    // A transfer fill may only use the exact CPU-owned argument snapshot
                    // already bound to this dispatch, never a later read of guest memory.
                    if (stage.pgm_hash == 0x8aa6e00c && !desc.is_written && size >= 12) {
                        for (const auto& chunk : stream.chunks) {
                            if (chunk.buffer->Mapped() &&
                                vk::Buffer{Handle<VkBuffer_T>(chunk.buffer->NativeHandle())} ==
                                    found->first && found->second <= chunk.buffer->Size() &&
                                12 <= chunk.buffer->Size() - found->second) {
                                std::array<u32, 3> words{};
                                std::memcpy(words.data(), chunk.buffer->Mapped() + found->second,
                                            sizeof(words));
                                b.fill_arguments_snapshot = words;
                                break;
                            }
                        }
                    }
                    const u64 aligned = Common::AlignDown(found->second, alignment);
                    const u64 adjust = found->second - aligned;
                    b.push.AddOffset(binding.buffer, static_cast<u32>(adjust));
                    const u64 bound_size = freeze_fill_args ? 12 : size;
                    info = {found->first, aligned, bound_size + adjust};
                    b.writes_buffers |= desc.is_written;
                    if (InPlaceSampled()) {
                        in_place.Note(BoundKind(desc.is_written ? 4 : 3), base, size); // NAT-142
                    }
                    if (desc.is_written) {
                        b.gpu_writes.emplace_back(base, size); // NAT-119
                    }
                }
            }
        }
        if(SceneTrace::Id())r.FrameDumpEvent(fmt::format("scene-buffer id={} stage={} program={:#x} binding={} descriptor={} special={} written={} handle={:#x} offset={} range={} direct={}",SceneTrace::Id(),u32(stage.sw_stage),stage.pgm_hash,binding.unified,buffer_index,desc.IsSpecial(),desc.is_written,reinterpret_cast<u64>(static_cast<VkBuffer>(info.buffer)),info.offset,info.range,stage.uses_direct));
        b.buffers.push_back(info);
        b.write_sets.push_back(u8(Shader::Backend::StageSet(u32(stage.sw_stage)))); // SHD-002
        b.writes.push_back(vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding.unified++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &b.buffers.back(),
        });
        ++binding.buffer;
    }
    (void)compute;
}

void NativeRenderer::BindTextures(const Shader::Info& stage, const AmdGpu::Regs& regs,
                                  Shader::Backend::Bindings& binding, DrawBindings& b) {
    if(SceneTrace::Id()) {
        std::string constants;
        for(size_t i=0;i<std::min<size_t>(stage.flattened_ud_buf.size(),128);++i)constants+=fmt::format(" {}:{:08x}",i,stage.flattened_ud_buf[i]);
        r.FrameDumpEvent(fmt::format("scene-stage id={} stage={} program={:#x} image_count={} sampler_count={} scalar_words={} captured_words={} values{}",SceneTrace::Id(),u32(stage.sw_stage),stage.pgm_hash,stage.images.size(),stage.samplers.size(),stage.flattened_ud_buf.size(),std::min<size_t>(stage.flattened_ud_buf.size(),128),constants));
    }
    // Local, F8-only Autopolis cascade-consumer trace. Never changes bindings.
    const bool trace_cascade = r.FrameDumpCollecting() &&
        stage.sw_stage == Shader::SwStage::Fragment &&
        std::ranges::any_of(stage.images, [&](const auto& image) {
            const auto address = image.GetSharp(stage).Address();
            return address == 0x100ca70000 || address == 0x2900000000 ||
                   address == 0x2920000000;
        });
    if (trace_cascade) {
        std::string values;
        for (size_t i = 0; i < std::min<size_t>(stage.flattened_ud_buf.size(), 384); ++i) {
            values += fmt::format(" {}:{:08x}@{:x}", i, stage.flattened_ud_buf[i],
                i < stage.flattened_ud_src.size() ? stage.flattened_ud_src[i] : 0);
        }
        r.FrameDumpEvent(fmt::format("cascade-reader ps {:#x} indices {} instances {}:{}",
            stage.pgm_hash, regs.num_indices, regs.num_instances.NumInstances(), values));
        for (const auto& sampler : stage.samplers) {
            const auto sharp = sampler.GetSharp(stage);
            std::array<u32, sizeof(sharp) / sizeof(u32)> words{};
            std::memcpy(words.data(), &sharp, sizeof(sharp));
            std::string raw;
            for (const auto word : words) raw += fmt::format(" {:08x}", word);
            std::array<u32,4> color{};
            const auto address = regs.ta_bc_base.Address() +
                u64(sharp.border_color_ptr.Value()) * sizeof(color);
            const bool custom = sharp.border_color_type.Value() == AmdGpu::BorderColor::Custom;
            const bool readable = custom && Core::RytyMemory::GuardedCopy(
                color.data(), reinterpret_cast<const void*>(address), sizeof(color));
            r.FrameDumpEvent(fmt::format(
                "cascade-sampler ps {:#x} compare {} lod {}..{} bias {} border {:#x} "
                "custom {} readable {} rgba {:08x}/{:08x}/{:08x}/{:08x} raw{}",
                stage.pgm_hash, sampler.is_depth, sharp.MinLod(), sharp.MaxLod(),
                sharp.LodBias(), address, custom, readable, color[0],color[1],color[2],color[3],raw));
        }
    }
    // DIAG-207 (RYGT_NULL_TEXTURES=<hash>:<hex slot mask>): that program's chosen texture slots
    // are bound empty, to see which one carries a wrong colour. Recorder-side draws only.
    static const std::pair<u64, u64> null_textures = [] {
        const char* env = std::getenv("RYGT_NULL_TEXTURES");
        if (!env || !*env) {
            return std::pair<u64, u64>{0, 0};
        }
        char* end{};
        const u64 hash = std::strtoull(env, &end, 16);
        const u64 mask = end && *end == ':' ? std::strtoull(end + 1, nullptr, 16) : ~u64{0};
        return std::pair<u64, u64>{hash, mask};
    }();
    u32 slot = 0;
    for (const auto& image_desc : stage.images) {
        const bool null_slot = null_textures.first != 0 && stage.pgm_hash == null_textures.first &&
                               slot < 64 && ((null_textures.second >> slot) & 1);
        ++slot;
        auto tsharp = image_desc.GetSharp(stage);
        MatchExperimentalCompositionTexture(tsharp, stage, image_desc, *registry);
        const bool cube_sampling = image_desc.post_op == Shader::SharpFetchPostOp::NativeCubeSampling;
        const u32 num_bindings = image_desc.NumBindings(stage);
        const u32 first = static_cast<u32>(b.images.size());
        const bool storage = image_desc.is_written;
        const auto add_write = [&] {
            b.write_sets.push_back(u8(Shader::Backend::StageSet(u32(stage.sw_stage)))); // SHD-002
            b.writes.push_back(vk::WriteDescriptorSet{
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = binding.unified,
                .dstArrayElement = 0,
                .descriptorCount = num_bindings,
                .descriptorType =
                    storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage,
                .pImageInfo = &b.images[first],
            });
            binding.unified += num_bindings;
        };
        const auto bind_null = [&](u32 reason) {
            if(SceneTrace::Id())r.FrameDumpEvent(fmt::format("scene-null-image id={} stage={} program={:#x} binding={} reason={} address={:#x}",SceneTrace::Id(),u32(stage.sw_stage),stage.pgm_hash,binding.unified,reason,tsharp.Address()));
            for (u32 i = 0; i < num_bindings; ++i) {
                b.images.push_back({VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral});
            }
            ++stats.null_images;
            ++stats.null_reasons[reason];
            add_write();
        };
        if (null_slot) {
            static std::atomic<u32> logged{};
            if (logged++ < 16) {
                LOG_WARNING(Render_Vulkan, "DIAG-207: {:#x} texture slot {} bound empty",
                            stage.pgm_hash, slot - 1);
            }
            bind_null(0);
            continue;
        }
        // OPT-002: the picture and views this T# resolved to last time.
        ++stats.texture_lookups;
        TextureMemo* memo = nullptr;
        std::array<u64, 4> raw_sharp{};
        static_assert(sizeof(tsharp) == sizeof(raw_sharp));
        std::memcpy(raw_sharp.data(), &tsharp, sizeof(raw_sharp));
        const u32 memo_flags = u32(storage) | (u32(image_desc.is_array) << 1) |
                               (u32(image_desc.is_depth) << 2) |
                               (u32(image_desc.mip_fallback_mode) << 3) |
                               ((image_desc.constant_mip_index & 0xff) << 6) |
                               (num_bindings << 16) | (u32(cube_sampling) << 31);
        if (num_bindings <= 4 && stage.pgm_hash != 0xaa3822a3 && !r.FrameDumpCollecting()) {
            memo = &texture_memos[XXH3_64bits_withSeed(raw_sharp.data(), sizeof(raw_sharp),
                                                       memo_flags) &
                                  (texture_memos.size() - 1)];
            if (memo->entry && memo->flags == memo_flags && memo->sharp == raw_sharp &&
                memo->generation == registry->Generation() + unmap_generation) {
                Native::ImageEntry* entry = memo->entry;
                entry->last_use.store(frame, std::memory_order_relaxed);
                if (Native::WatchedTexture(entry->info.guest_address,
                                           entry->info.guest_address + 1)) {
                    WatchBind(*entry, tsharp, storage, true, stage.pgm_hash); // FIX-157
                }
                ApplyRewrite(*entry); // NAT-120
                if (!storage && entry->from_cpu) {
                    RefreshSources(*entry); // NAT-104
                }
                for (u32 i = 0; i < num_bindings; ++i) {
                    b.images.push_back({VK_NULL_HANDLE, memo->views[i], vk::ImageLayout::eGeneral});
                }
                (storage ? b.stored : b.sampled).push_back(entry);
                add_write();
                ++stats.texture_memo_hits;
                continue;
            }
        }
        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid ||
            !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt)) {
            bind_null(0);
            continue;
        }
        if (!r.memory->IsValidGpuMapping(tsharp.Address(), 0)) {
            bind_null(1);
            continue;
        }
        if (LiverpoolToVK::TrySurfaceFormat(data_fmt, num_fmt) == vk::Format::eUndefined) {
            bind_null(2);
            continue;
        }
        if (VideoCore::CheckImageDescriptorGeometry(tsharp, r.instance.GetImageLimits()) !=
            VideoCore::ImageDescriptorGeometryError::None) {
            bind_null(3);
            continue;
        }
        VideoCore::TextureCache::ImageDesc desc{tsharp, image_desc};
        const bool original_backing=HasImageBacking(desc.info.guest_address,desc.info.guest_size);
        const u64 original_address=desc.info.guest_address;
        const u32 original_first_mip=desc.view_info.range.base.level;
        const bool resident_view=!original_backing && !storage && !cube_sampling && num_bindings==1 &&
            image_desc.mip_fallback_mode==Shader::MipStorageFallbackMode::None &&
            RebaseResidentPrtView(desc,[&](u64 address,u64 bytes){return HasImageBacking(address,bytes);});
        if (resident_view && SceneTrace::Id()) r.FrameDumpEvent(fmt::format(
            "scene-prt-resident-view id={} program={:#x} original={:#x} first_mip={} resolved={:#x} bytes={} size={}x{} levels={}",
            SceneTrace::Id(),stage.pgm_hash,original_address,original_first_mip,desc.info.guest_address,
            desc.info.guest_size,desc.info.size.width,desc.info.size.height,desc.info.resources.levels));
        const auto trace_rejected_backing = [&](const char* failure) {
#ifdef _WIN32
            if (!SceneTrace::Id()) return;
            static thread_local u64 observed_frame=~u64{};
            static thread_local std::vector<std::pair<u64,u64>> seen;
            if (observed_frame!=frame) { observed_frame=frame; seen.clear(); }
            const std::pair<u64,u64> key{desc.info.guest_address,desc.info.guest_size};
            if (std::find(seen.begin(),seen.end(),key)!=seen.end() || seen.size()>=64) return;
            seen.push_back(key);
            const auto snapshot=SceneResidency::Observe(key.first,key.second,[](u64 address,SceneResidency::Region& region) {
                MEMORY_BASIC_INFORMATION info{};
                if (!VirtualQuery(reinterpret_cast<const void*>(address),&info,sizeof(info))) return false;
                const auto begin=reinterpret_cast<u64>(info.BaseAddress);
                if (info.RegionSize>std::numeric_limits<u64>::max()-begin) return false;
                region={begin,begin+info.RegionSize,info.State,info.Protect,info.Type};
                return true;
            });
            const auto total=SceneResidency::Cover(snapshot,key.first,key.second);
            r.FrameDumpEvent(fmt::format("scene-rejected-backing id={} failure={} program={:#x} binding={} address={:#x} bytes={} tile={} prt={} size={}x{} levels={} layers={} view_base_mip={} view_last_mip={} min_lod={} cached_image={} observed={} committed={} complete={} query_failed={} invalid={} raw={:016x}/{:016x}/{:016x}/{:016x}",
                SceneTrace::Id(),failure,stage.pgm_hash,binding.unified,key.first,key.second,
                u32(desc.info.tile_mode),AmdGpu::IsPrt(desc.info.array_mode),desc.info.size.width,desc.info.size.height,
                desc.info.resources.levels,desc.info.resources.layers,u32(tsharp.base_level),u32(tsharp.last_level),
                float(tsharp.min_lod)/256.0f,registry->HasImageAt(key.first),total.observed,total.committed,snapshot.Complete(),
                snapshot.query_failed,snapshot.invalid,raw_sharp[0],raw_sharp[1],raw_sharp[2],raw_sharp[3]));
            for (const auto& region:snapshot.regions) {
                r.FrameDumpEvent(fmt::format("scene-backing-region id={} address={:#x} begin={:#x} end={:#x} state={:#x} protect={:#x} type={:#x}",
                    SceneTrace::Id(),key.first,region.begin,region.end,region.state,region.protect,region.type));
            }
            for (u32 mip=0;mip<std::min<u32>(desc.info.resources.levels,desc.info.mips_layout.size());++mip) {
                const auto& layout=desc.info.mips_layout[mip];
                const auto coverage=SceneResidency::Cover(snapshot,key.first+layout.offset,layout.size);
                r.FrameDumpEvent(fmt::format("scene-backing-mip id={} address={:#x} mip={} offset={} bytes={} observed={} committed={} complete={}",
                    SceneTrace::Id(),key.first,mip,layout.offset,layout.size,coverage.observed,coverage.committed,coverage.complete));
            }
#endif
        };
        // A valid base address is not sufficient: stale optional descriptors can
        // span gigabytes beyond their allocation. Reject before image creation,
        // guest-memory import, staging allocation or a detiling dispatch.
        if (!original_backing && !resident_view) {
            trace_rejected_backing("span");
            static std::atomic<u32> logged{};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
                LOG_WARNING(Render_Vulkan,
                    "IMAGE-BACKING: rejected ps/vs {:#x} image {:#x}+{:#x} {}x{}x{} pitch {} "
                    "mips {} layers {} tile {} bits {}; raw {:016x} {:016x} {:016x} {:016x}",
                    stage.pgm_hash, desc.info.guest_address, desc.info.guest_size,
                    desc.info.size.width, desc.info.size.height, desc.info.size.depth,
                    desc.info.pitch, desc.info.resources.levels, desc.info.resources.layers,
                    u32(desc.info.tile_mode), desc.info.num_bits,
                    raw_sharp[0], raw_sharp[1], raw_sharp[2], raw_sharp[3]);
            }
            bind_null(4);
            continue;
        }

        // FIX-046: an image must start inside the guest address space. FIX-081: GT Sport's grass
        // shader reads garbage T#s from one slot when the texture is unused; from there only an
        // image the game already made is bound.
        if (!r.IsMappedStart(desc.info.guest_address)) {
            trace_rejected_backing("start");
            bind_null(4);
            continue;
        }
        if (stage.pgm_hash == 0xaa3822a3 && image_desc.sharp_fetch.offsets[0] == 109 &&
            !registry->HasImageAt(desc.info.guest_address)) {
            bind_null(5);
            continue;
        }
        Native::ImageEntry* entry = registry->Find(desc.info, storage, frame);
        if (!entry) {
            bind_null(6);
            continue;
        }
        // A sampled-only fallback must never become a writable storage image.
        if (storage && !(vk::ImageUsageFlags{entry->backend_usage} &
                         vk::ImageUsageFlagBits::eStorage)) {
            bind_null(7);
            continue;
        }
        if (Native::WatchedTexture(desc.info.guest_address, desc.info.guest_address + 1)) {
            WatchBind(*entry, tsharp, storage, false, stage.pgm_hash); // FIX-157 (diagnosis)
        }
        ApplyRewrite(*entry); // NAT-120 (NAT-133: textures too)
        if (!storage && r.FrameDumpCollecting()) {
            // FIX-152 (diagnosis): RYGT_FRAME_DUMP_ALL_TEXTURES=1 dumps uploaded ones too.
            static const bool dump_all = [] {
                const char* env = std::getenv("RYGT_FRAME_DUMP_ALL_TEXTURES");
                return env && env[0] == '1';
            }();
            if (dump_all || !entry->from_cpu || SceneTrace::Id()) {
                auto key = Rasterizer::FrameDumpTextureKey(desc.info); // NAT-123
                if (std::ranges::find(dump_textures, key, &decltype(dump_textures)::value_type::first) ==
                    dump_textures.end()) {
                    dump_textures.emplace_back(std::move(key), entry);
                }
            }
        }
        if (!storage && entry->from_cpu) {
            // (part of the textures' time; reported apart; OPT-001: on timed draws only)
            PhaseClock sources_clock{stats.phase_ns[4]};
            RefreshSources(*entry); // NAT-104
        }
        const auto fallback = image_desc.mip_fallback_mode;
        for (u32 i = 0; i < num_bindings; ++i) {
            auto view_info = desc.view_info;
            if (fallback == Shader::MipStorageFallbackMode::ConstantIndex) {
                view_info.range.base.level += image_desc.constant_mip_index;
                view_info.range.extent.levels = 1;
            } else if (fallback == Shader::MipStorageFallbackMode::DynamicIndex) {
                view_info.range.base.level += i;
                view_info.range.extent.levels = 1;
            }
            const vk::ImageView view =
                View(*entry, view_info, storage ? ViewUse::Storage
                                               : cube_sampling ? ViewUse::CubeSampled
                                                           : ViewUse::Sampled);
            if(SceneTrace::Id())r.FrameDumpEvent(fmt::format("scene-image id={} stage={} program={:#x} binding={} element={} storage={} comparison={} requested={:#x} actual={:#x} format={} size={}x{} levels={} layers={} view_mip={}+{} view_layer={}+{} min_lod={} fallback={} version={} dirty={} from_cpu={}",SceneTrace::Id(),u32(stage.sw_stage),stage.pgm_hash,binding.unified,i,storage,image_desc.is_depth,desc.info.guest_address,entry->info.guest_address,u32(entry->info.pixel_format),entry->info.size.width,entry->info.size.height,entry->info.resources.levels,entry->info.resources.layers,view_info.range.base.level,view_info.range.extent.levels,view_info.range.base.layer,view_info.range.extent.layers,view_info.min_lod,u32(fallback),entry->write_version.load(),entry->needs_upload.load(),entry->from_cpu));
            if (trace_cascade) {
                r.FrameDumpEvent(fmt::format(
                    "cascade-view ps {:#x} requested {:#x} {}x{} mips {} layers {} "
                    "actual {:#x} {}x{} mips {} layers {} from-cpu {} depth {} "
                    "view mip {}+{} layer {}+{} min-lod {} comparison {} raw {:016x}/{:016x}/{:016x}/{:016x} "
                    "version {} uploads {} dirty {} kept {} tracked {} guest-modified {}",
                    stage.pgm_hash, desc.info.guest_address, desc.info.size.width, desc.info.size.height,
                    desc.info.resources.levels, desc.info.resources.layers, entry->info.guest_address,
                    entry->info.size.width, entry->info.size.height, entry->info.resources.levels,
                    entry->info.resources.layers, entry->from_cpu, bool(entry->info.props.is_depth),
                    view_info.range.base.level, view_info.range.extent.levels,
                    view_info.range.base.layer, view_info.range.extent.layers, view_info.min_lod,
                    image_desc.is_depth, raw_sharp[0],raw_sharp[1],raw_sharp[2],raw_sharp[3],
                    entry->write_version.load(),entry->total_uploads.load(),entry->needs_upload.load(),
                    entry->kept.load(),entry->guest_writes_tracked,entry->guest_modified.load()));
            }
            b.images.push_back({VK_NULL_HANDLE, view, vk::ImageLayout::eGeneral});
        }
        if (memo) {
            // OPT-002: remembered for the next draws reading this T# the same way.
            memo->sharp = raw_sharp;
            memo->flags = memo_flags;
            memo->entry = entry;
            for (u32 i = 0; i < num_bindings; ++i) {
                memo->views[i] = b.images[first + i].imageView;
            }
            memo->generation = registry->Generation() + unmap_generation;
        }
        (storage ? b.stored : b.sampled).push_back(entry);
        add_write();
    }
    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (!ssharp.Valid() || (ssharp.border_color_type.Value() == AmdGpu::BorderColor::Custom &&
                                regs.ta_bc_base.Address() == 0)) {
            ssharp = AmdGpu::Sampler{};
        }
        if(SceneTrace::Id())r.FrameDumpEvent(fmt::format("scene-sampler id={} stage={} program={:#x} binding={} comparison={} compare_op={} min_lod={} max_lod={} original_bias={} sanitized_bias={} effective_bias={} effective_max_lod={} mip_filter={} min_filter={} mag_filter={} anisotropy={} clamp={}/{}/{} border={}",SceneTrace::Id(),u32(stage.sw_stage),stage.pgm_hash,binding.unified,sampler.is_depth,u32(ssharp.depth_compare_func.Value()),ssharp.MinLod(),ssharp.MaxLod(),sampler.GetSharp(stage).LodBias(),ssharp.LodBias(),std::min(ssharp.LodBias(),r.instance.MaxSamplerLodBias()),std::max(ssharp.MinLod(),ssharp.MaxLod()),u32(ssharp.mip_filter.Value()),u32(ssharp.xy_min_filter.Value()),u32(ssharp.xy_mag_filter.Value()),ssharp.MaxAniso(),u32(ssharp.clamp_x.Value()),u32(ssharp.clamp_y.Value()),u32(ssharp.clamp_z.Value()),u32(ssharp.border_color_type.Value())));
        b.images.push_back({SamplerFor(ssharp, regs.ta_bc_base, sampler.is_depth), VK_NULL_HANDLE,
                            vk::ImageLayout::eGeneral});
        b.write_sets.push_back(u8(Shader::Backend::StageSet(u32(stage.sw_stage)))); // SHD-002
        b.writes.push_back(vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding.unified++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampler,
            .pImageInfo = &b.images.back(),
        });
    }
}

void NativeRenderer::PushBindings(const Pipeline& pipeline, vk::PipelineBindPoint bind_point,
                                  DrawBindings& b) {
    auto* pending_owner=FindParallelRuntime(this);
    if(!parallel_binding_capture && pending_owner)
        pending_owner->descriptor_updates.Flush(r.instance.GetDevice(),PendingDescriptorUpdates::Serial);
    Native::SlowCallTrace<9> trace{t_timed, "descriptor-bindings",
        {"push-constants", "groups-keys", "push-descriptor", "hash-lookup", "allocate",
         "copy-writes", "update-descriptors", "store-key", "bind-set"}};
    Stream& s = stream;
    const CmdBuf cmdbuf{s.cmd};
    const auto stage_flags =
        pipeline.IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    if(!parallel_binding_capture) cmdbuf.pushConstants(pipeline.GetLayout(), stage_flags, 0u, sizeof(b.push), &b.push);
    if (b.writes.empty()) {
        return;
    }
    // SHD-002: each stage's writes go to its own set; one set may be pushed.
    trace.Next(1);
    if (!b.groups_ready) b.PrepareWriteGroups();
    const auto& by_set = b.grouped_writes;
    if (b.key_layout != pipeline.GetLayout()) b.PrepareDescriptorKeys(pipeline);
    const vk::Device vk_device = r.instance.GetDevice();
    static thread_local boost::container::small_vector<vk::WriteDescriptorSet, 64> updates;
    updates.clear();
    std::array<vk::DescriptorSet, Shader::Backend::MaxDescriptorSets> sets{};
    PendingDescriptorUpdates* pending=parallel_binding_capture && pending_owner ? &pending_owner->descriptor_updates : nullptr;
    bool deferred=false;u64 staged_before=0;
    if(pending) {
        const auto check=[&] {
            const auto buffers=std::span<const vk::DescriptorBufferInfo>{b.buffers.data(),b.buffers.size()};
            const auto images=std::span<const vk::DescriptorImageInfo>{b.images.data(),b.images.size()};
            if(parallel_binding_capture->counts_proven)
                return pending->CheckCounts(parallel_binding_capture->ordinary_writes,parallel_binding_capture->ordinary_buffers,parallel_binding_capture->ordinary_images);
            return parallel_binding_capture->write_structure_proven
                ? pending->Check<true>(by_set,pipeline.PushSet(),buffers,images)
                : pending->Check(by_set,pipeline.PushSet(),buffers,images);
        };
        auto admission=check();
        if(admission==PendingDescriptorUpdates::Admission::Capacity) {
            ++pending->overflow_draws;
            pending->Flush(vk_device,PendingDescriptorUpdates::Capacity);
            admission=check(); // revalidate all rows; capacity may have stopped the first scan
        }
        deferred=admission==PendingDescriptorUpdates::Admission::Yes;
        if(!deferred) {
            const bool overflow=admission==PendingDescriptorUpdates::Admission::Capacity;
            if(!overflow)++pending->unsupported_draws;
            pending->Flush(vk_device,overflow?PendingDescriptorUpdates::Capacity:PendingDescriptorUpdates::Unsupported);
        }
        staged_before=pending->staged_rows;
    }
    const bool alloc_diag = DescriptorAllocDiagEnabled();
    const unsigned alloc_kind = pipeline.IsCompute() ? 1 : 0;
    u32 alloc_misses = 0;
    if (alloc_diag && descriptor_alloc_owner != this) { descriptor_alloc_metrics = {}; std::fill_n(descriptor_bind_sets, 2, 0); std::fill_n(descriptor_bind_runs, 2, 0); std::fill_n(descriptor_bind_multi, 2, 0); descriptor_alloc_owner = this; }
    try { for (u32 index = 0; index < by_set.size(); ++index) {
        auto& writes = by_set[index];
        if (writes.empty()) {
            continue;
        }
        if (index == pipeline.PushSet()) {
            continue; // recorded in the original set order after host updates
        }
        const vk::DescriptorSetLayout layout = pipeline.SetLayout(index);
        trace.Next(3, index, writes.size());
        // Cache values, never pointers into per-draw binding storage. Rebind even
        // on a hit so pipeline-layout switches and intervening passes remain safe.
        auto& prepared_key = b.descriptor_keys[index];
        // Preparation hashed the complete key. Commit changes only buffer slots;
        // recompute the same XXH3 hash only when those values actually changed.
        const bool key_changed = prepared_key.RefreshBuffers<false>();
        const auto& key = prepared_key.words;
        const bool cacheable = prepared_key.cacheable;
        const std::span<const u64> descriptor_key{key.data(), key.size()};
        if (cacheable && key_changed) {
            prepared_key.hash = Native::Parallel::AdaptiveXXH3(key.data(), key.size() * sizeof(u64));
            ++descriptor_hash_refreshed;
        } else if (cacheable) {
            ++descriptor_hash_reused;
        }
        const u64 key_hash = cacheable ? prepared_key.hash : 0;
        if (cacheable) {
            if (const auto reused = s.descriptor_sets.Find(key_hash, descriptor_key)) {
                sets[index] = reused;
                ++stats.descriptor_reuses;
                if (alloc_diag) ++descriptor_alloc_metrics.cache_hits[alloc_kind];
                continue;
            }
        }
        trace.Next(4);
        const u64 layout_key = u64(VkDescriptorSetLayout(layout));
        vk::DescriptorSet set{};
        if(parallel_binding_capture && parallel_binding_capture->ahead) {
            auto* ahead=parallel_binding_capture->ahead;
            if(ahead->pipeline_layout==pipeline.GetLayout() && ahead->layouts[index]==layout && ahead->sets[index]) {
                set=ahead->sets[index];++ahead->used_sets;ahead->context->used_sets.fetch_add(1,std::memory_order_relaxed);parallel_binding_capture->ahead_used=true;
            }
        }
        const bool ahead_hit=bool(set);
        if(!set)set=s.descriptor_reserve.Take(layout_key);
        const bool reserve_hit=bool(set) && !ahead_hit;
        if(reserve_hit)++stats.descriptor_reserve_hits;
        if (alloc_diag) { if (reserve_hit) ++descriptor_alloc_metrics.reserve_hits[alloc_kind]; else if(!set) ++alloc_misses; }
        if(!set) {
        std::array<vk::DescriptorSetLayout, 32> layouts;
        layouts.fill(layout);
        std::array<vk::DescriptorSet, 32> allocated{};
        u32 allocate_count = u32(s.descriptor_reserve.BatchSize(layout_key));
        for (u32 attempt = 0; attempt < 3 && !set; ++attempt) {
            if (s.pools.empty() || attempt == 2) {
                if (alloc_diag) { if (s.pools.empty()) ++descriptor_alloc_metrics.pool_initial; else ++descriptor_alloc_metrics.pool_rotate; }
                s.pools.push_back(TakeDescriptorPool());
            }
            // A block may not fit: try one in the same pool, then one in a new pool.
            if (attempt != 0) allocate_count = 1;
            const vk::DescriptorSetAllocateInfo alloc{
                .descriptorPool = s.pools.back(),
                .descriptorSetCount = allocate_count,
                .pSetLayouts = layouts.data(),
            };
            ++stats.descriptor_allocate_calls;
            const bool sampled = alloc_diag && descriptor_alloc_metrics.NextSample();
            u64 cpu_before=0, cpu_after=0;
            const bool cpu_started = sampled && DescriptorThreadCpu100ns(cpu_before);
            const auto wall_start = sampled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const auto allocation_result = vk_device.allocateDescriptorSets(&alloc, allocated.data());
            if (alloc_diag) {
                u64 wall_ns=0; bool cpu_valid=false;
                if (sampled) {
                    wall_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-wall_start).count();
                    cpu_valid=cpu_started && DescriptorThreadCpu100ns(cpu_after) && cpu_after >= cpu_before;
                }
                const auto result = allocation_result == vk::Result::eSuccess ? Native::DescriptorResult::Success :
                    (allocation_result == vk::Result::eErrorOutOfPoolMemory || allocation_result == vk::Result::eErrorFragmentedPool)
                        ? Native::DescriptorResult::Capacity : Native::DescriptorResult::Other;
                descriptor_alloc_metrics.Attempt(allocate_count > 1, attempt, result, sampled, wall_ns, cpu_valid, cpu_valid ? cpu_after-cpu_before : 0);
            }
            if (allocation_result == vk::Result::eSuccess) {
                set = allocated[0];
                stats.descriptor_allocations += allocate_count;
                if (allocate_count > 1) s.descriptor_reserve.Store(layout_key,
                    std::span<const vk::DescriptorSet>{allocated.data() + 1, allocate_count - 1});
            }
        }
        }
        if (!set) {
            static std::atomic<u32> logged{};
            if (Sample(logged)) {
                LOG_ERROR(Render_Vulkan, "NAT-100: no descriptor set for a draw");
            }
            continue;
        }
        // A miss needs mutable destinations. Hits and push sets borrow the original
        // writes directly; never leave a destination handle in per-draw storage.
        trace.Next(5);
        if(deferred)pending->Stage(writes,set);
        else Native::AppendDescriptorUpdates(writes,set,updates);
        trace.Next(7);
        if (cacheable) s.descriptor_sets.Store(key_hash, descriptor_key, set);
        sets[index] = set;
    }} catch(...) {
        // A partially built immediate draw may already have published earlier sets.
        // Keep its borrowed payload alive until all such writes have been applied.
        if(pending) {
            pending->Flush(vk_device,PendingDescriptorUpdates::Exception);
            if(!updates.empty()) {vk_device.updateDescriptorSets(updates,{});++pending->actual_calls;}
        }
        throw;
    }
    if (alloc_diag) descriptor_alloc_metrics.Draw(alloc_kind, alloc_misses);
    if(parallel_binding_capture) {
        if(deferred && pending->staged_rows!=staged_before)++pending->logical_draws;
        if(!updates.empty()) {trace.Next(6);vk_device.updateDescriptorSets(updates,{});
            if(pending){++pending->actual_calls;++pending->logical_draws;++pending->immediate_draws;}}
        parallel_binding_capture->sets=sets;
        return;
    }
    std::array<vk::DescriptorSet, Shader::Backend::MaxDescriptorSets> bind_run{};
    u32 bind_first = 0, bind_count = 0;
    const auto flush_bind_run = [&] {
        if (!bind_count) return;
        trace.Next(8);
        cmdbuf.bindDescriptorSets(bind_point, pipeline.GetLayout(), bind_first,
                                  vk::ArrayProxy<const vk::DescriptorSet>{bind_count, bind_run.data()}, {});
        if (alloc_diag) { descriptor_bind_sets[alloc_kind] += bind_count; ++descriptor_bind_runs[alloc_kind]; if (bind_count > 1) ++descriptor_bind_multi[alloc_kind]; }
        bind_count = 0;
    };
    Native::CommitDescriptorBatch(updates,sets,pipeline.PushSet(),by_set,
        [&](const auto& all) {trace.Next(6);vk_device.updateDescriptorSets(all,{});},
        [&](u32 index,const auto& writes) {
            flush_bind_run();
            trace.Next(2);cmdbuf.pushDescriptorSetKHR(bind_point,pipeline.GetLayout(),index,writes);
        },
        [&](u32 index,vk::DescriptorSet set) {
            if (bind_count && index != bind_first + bind_count) flush_bind_run();
            if (!bind_count) bind_first = index;
            bind_run[bind_count++] = set;
        });
    flush_bind_run();
}

bool NativeRenderer::MirrorKind(char kind) {
    // FIX-177 (diagnosis, RYGT_NATIVE_MIRRORS=v or i): only vertex or only index ranges mirrored.
    static const char only = [] {
        const char* env = std::getenv("RYGT_NATIVE_MIRRORS");
        return env && (env[0] == 'v' || env[0] == 'i') ? env[0] : '\0';
    }();
    return only == '\0' || only == kind;
}

void NativeRenderer::ResolveVertexBuffers(const GraphicsPipeline& pipeline,
                                          const AmdGpu::Regs& regs, VertexIndexState& v) {
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline.GetVertexInputs(v.attributes, v.input_bindings, divisors, guest_buffers,
                             regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);
    // OPT-004: a draw's attributes mostly read one mesh (~12 V#s a draw in GT Sport races): a
    // range inside the mirror the previous attribute was bound from, while it is there with
    // nothing to copy in, is bound from it without a lookup (what the lookup would return).
    Mirror* near = nullptr;
    u64 near_generation = 0;
    const bool sampled = InPlaceSampled();
    for (u32 index = 0; index < guest_buffers.size(); ++index) {
        const auto& guest = guest_buffers[index];
        std::optional<std::pair<vk::Buffer, u64>> found;
        u64 size = 0;
        const VAddr base = guest.base_address;
        const u64 sharp_size = guest.GetSize();
        // FIX-178 (diagnosis, RYGT_NATIVE_VERTEX_NEAR=0): every attribute looked up.
        static const bool near_on = [] {
            const char* env = std::getenv("RYGT_NATIVE_VERTEX_NEAR");
            return !(env && env[0] == '0');
        }();
        if (near_on && near && !sampled && base != 0 && sharp_size > 0 &&
            near_generation == buffer_generation && base >= near->begin &&
            base + sharp_size <= near->end && near->dirty_pages.empty()) {
            size = sharp_size;
            found = std::pair{vk::Buffer{Handle<VkBuffer_T>(near->buffer->NativeHandle())},
                              u64{base - near->begin}};
            ++stats.mirror_binds;
            ++stats.vertex_near;
        } else if (base != 0 && sharp_size > 0 && r.memory->MappedAreaEnd(base, base + 1) != 0) {
            size = r.memory->ClampRangeSize(base, sharp_size);
            found = GuestBuffer(base, size, false, MirrorKind('v')); // FIX-177
            if (sampled) {
                in_place.Note(BoundKind(0), base, size); // NAT-142
            }
            near = found ? last_mirror : nullptr;
            near_generation = buffer_generation;
        }
        v.buffers.push_back(found ? found->first : vk::Buffer{});
        v.offsets.push_back(found ? found->second : 0);
        v.sizes.push_back(found ? size : 0);
        v.strides.push_back(guest.GetStride());
        Mirror* bound_mirror = found && near && base >= near->begin &&
                                       base + std::max<u64>(size, 1) <= near->end
                                   ? near
                                   : nullptr;
        if (bound_mirror && found->first == vk::Buffer{Handle<VkBuffer_T>(
                                                bound_mirror->buffer->NativeHandle())}) {
            CheckMirrorBind(*bound_mirror, base, size); // FIX-179 (diagnosis)
        }
    }
}

void NativeRenderer::CheckVertexReach(const GraphicsPipeline& pipeline, const AmdGpu::Regs& regs,
                                      bool is_indexed, u32 index_offset) {
    // FIX-181 (diagnosis, RYGT_NATIVE_VERTEX_REACH_CHECK=<n>): every n-th draw, the highest
    // vertex it can read (its indices read from guest memory, or its vertex count) against each
    // vertex V#'s end and the mirror bound there: draws reading past a mirror's end got zeros
    // (the tunnel explosions went away with mirrors 256 KB longer).
    static const u32 every = [] {
        const char* env = std::getenv("RYGT_NATIVE_VERTEX_REACH_CHECK");
        return env && *env ? u32(std::strtoul(env, nullptr, 10)) : 0u;
    }();
    if (every == 0 || ++reach_check_counter % every != 0) {
        return;
    }
    VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline.GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                             regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);
    if (guest_buffers.empty()) {
        return;
    }
    const auto [vertex_offset, instance_offset] =
        DrawOffsets(regs, pipeline.GetStage(Shader::SwStage::Vertex), pipeline.GetFetchShader());
    u64 max_index = 0;
    const u32 count = regs.num_indices;
    if (is_indexed) {
        const bool is16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
        const u32 index_size = is16 ? 2 : 4;
        const VAddr address =
            regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size;
        const u64 bytes = std::min<u64>(u64(count) * index_size, 1_MB);
        static thread_local std::vector<u8> indices;
        indices.resize(bytes);
        if (bytes == 0 ||
            !Core::RytyMemory::GuardedCopy(indices.data(), reinterpret_cast<const void*>(address),
                                           bytes)) {
            return;
        }
        const bool restart = regs.enable_primitive_restart & 1;
        const u32 restart_index = regs.primitive_restart_index;
        for (u64 i = 0; i < bytes / index_size; ++i) {
            const u32 value = is16 ? u32(reinterpret_cast<const u16*>(indices.data())[i])
                                   : reinterpret_cast<const u32*>(indices.data())[i];
            if (restart && value == (is16 ? (restart_index & 0xffff) : restart_index)) {
                continue;
            }
            max_index = std::max<u64>(max_index, value);
        }
    } else {
        max_index = count == 0 ? 0 : count - 1;
    }
    ++stats.reach_checks;
    for (u32 i = 0; i < guest_buffers.size(); ++i) {
        const auto& guest = guest_buffers[i];
        const u64 stride = guest.GetStride();
        if (stride == 0) {
            continue;
        }
        const bool per_instance = bindings[i].inputRate == vk::VertexInputRate::eInstance;
        const u64 divisor = per_instance ? std::max<u32>(bindings[i].divisor, 1) : 1;
        const u64 instances = std::max<u32>(regs.num_instances.NumInstances(), 1);
        const u64 last = per_instance ? u64(instance_offset) + (instances - 1) / divisor
                                      : u64(vertex_offset) + max_index;
        const VAddr base = guest.base_address;
        const VAddr sharp_end = base + guest.GetSize();
        const VAddr reach = base + (last + 1) * stride;
        if (reach <= sharp_end) {
            continue;
        }
        ++stats.reach_past_sharp;
        const Mirror* mirror = MirrorHolding(base, 1);
        const bool past_mirror = mirror && reach > mirror->end;
        stats.reach_past_mirror += past_mirror ? 1 : 0;
        static std::atomic<u32> logged{};
        if (Sample(logged)) {
            LOG_WARNING(Render_Vulkan,
                        "FIX-181: draw (vs {:#x}) binding {} ({}) V# {:#x} stride {} records {} "
                        "reads to element {} (vertex offset {}, max index {}, instance offset {}, "
                        "{} instances): {:#x} past the V#'s end{}",
                        pipeline.GetStage(Shader::SwStage::Vertex).pgm_hash, i,
                        per_instance ? "per instance" : "per vertex", base, stride,
                        guest.num_records, last, vertex_offset, max_index, instance_offset,
                        instances, reach - sharp_end,
                        mirror ? fmt::format(", mirror {:#x}+{:#x}{}", mirror->begin,
                                             mirror->end - mirror->begin,
                                             past_mirror ? " (past its end too)" : "")
                               : std::string{});
        }
    }
}

void NativeRenderer::CheckMirrorBind(Mirror& mirror, VAddr base, u64 size) {
    // FIX-179 (diagnosis, RYGT_NATIVE_MIRROR_CHECK=<n>): every n-th vertex range bound from a
    // mirror, the mirror's bytes and guest memory's bytes there are copied out side by side right
    // before the draw (the GPU's view of both at that point) and compared once the GPU is done.
    static const u32 every = [] {
        const char* env = std::getenv("RYGT_NATIVE_MIRROR_CHECK");
        return env && *env ? u32(std::strtoul(env, nullptr, 10)) : 0u;
    }();
    if (every == 0) {
        return;
    }
    const u64 done = r.scheduler.GetWorkSemaphore()->KnownGpuTick();
    for (auto it = mirror_checks.begin(); it != mirror_checks.end();) {
        if (it->tick > done) {
            ++it;
            continue;
        }
        const u8* a = it->buffer->Mapped();
        const u8* b = a + it->size;
        u64 first = it->size, last = 0, differ = 0;
        for (u64 i = 0; i < it->size; ++i) {
            if (a[i] != b[i]) {
                first = std::min(first, i);
                last = i;
                ++differ;
            }
        }
        ++stats.mirror_checks;
        if (differ != 0) {
            ++stats.mirror_check_differ;
            static std::atomic<u32> logged{};
            if (Sample(logged)) {
                LOG_WARNING(Render_Vulkan,
                            "FIX-179: vertex range {:#x}+{:#x} (frame {}): mirror {:#x}+{:#x} "
                            "differs from guest memory in {} bytes ({:#x}..{:#x}); mirror made or "
                            "copied in last at frame {}, {} dirty pages then",
                            it->base, it->size, it->frame, it->mirror_begin, it->mirror_size,
                            differ, first, last, it->mirror_used, it->dirty);
            }
        }
        it = mirror_checks.erase(it);
    }
    if (++mirror_check_counter % every != 0 || mirror_checks.size() >= 64) {
        return;
    }
    const u64 bytes = std::min<u64>(size, 16_KB) & ~u64{3};
    if (bytes == 0) {
        return;
    }
    auto* direct = r.buffer_cache.GetDirectMemory();
    if (!direct || !direct->EnsureImported(base, bytes)) {
        return;
    }
    auto buffer = device->CreateBuffer({.size = bytes * 2,
                                        .usage = Native::Rhi::BufferUsageStorage |
                                                 Native::Rhi::BufferUsageTransferDst,
                                        .memory = Native::Rhi::MemoryKind::Readback,
                                        .name = "mirror check"});
    if (!buffer || !buffer->Mapped()) {
        return;
    }
    Stream& s = Begin();
    GlobalBarrier();
    const vk::Buffer dst{Handle<VkBuffer_T>(buffer->NativeHandle())};
    s.cmd.copyBuffer(vk::Buffer{Handle<VkBuffer_T>(mirror.buffer->NativeHandle())}, dst,
                     vk::BufferCopy{.srcOffset = base - mirror.begin, .dstOffset = 0,
                                    .size = bytes});
    direct->ForEachPart(base, bytes,
                        [&](const VideoCore::Buffer& view, u64 view_offset, u64 offset,
                            u64 part) {
                            s.cmd.copyBuffer(view.Handle(), dst,
                                             vk::BufferCopy{.srcOffset = view_offset,
                                                            .dstOffset = bytes + offset,
                                                            .size = part});
                        });
    s.buffers_written = true;
    mirror_checks.push_back({std::move(buffer), r.scheduler.CurrentTick() + 1, bytes, base,
                             frame, mirror.begin, mirror.end - mirror.begin, mirror.used_frame,
                             mirror.dirty_pages.size()});
}

bool NativeRenderer::ResolveIndices(const AmdGpu::Regs& regs, bool is_indexed, u32 index_offset,
                                    bool quad_triangles, VertexIndexState& v) {
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    v.index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    v.index_count = regs.num_indices;
    if (quad_triangles) {
        const auto& corners =
            regs.polygon_control.provoking_vtx_last == AmdGpu::ProvokingVtxLast::Last
                ? QuadLastVertex
                : QuadFirstVertex;
        const u32 count = regs.num_indices;
        const u32 max_indices = count / 4 * 6;
        v.index_count = 0;
        if (max_indices == 0) {
            return true;
        }
        if (!is_indexed) {
            const auto [buffer, offset, data] = Allocate(u64(max_indices) * sizeof(u32), 4);
            auto* out = reinterpret_cast<u32*>(data);
            for (u32 quad = 0; quad + 4 <= count; quad += 4) {
                for (const u32 corner : corners) {
                    *out++ = quad + corner;
                }
            }
            v.index_buffer = buffer;
            v.index_offset = offset;
            v.index_type = vk::IndexType::eUint32;
            v.index_count = max_indices;
            return true;
        }
        const VAddr address =
            regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size;
        if (!Native::Readable(reinterpret_cast<const void*>(address), u64(count) * index_size)) {
            return false; // NAT-106: indices the CPU can't read
        }
        const bool restart = (regs.enable_primitive_restart & 1) != 0;
        const auto [buffer, offset, data] = Allocate(u64(max_indices) * index_size, 4);
        if (is_index16) {
            v.index_count = ExpandQuads<u16>(
                reinterpret_cast<const u16*>(address), count,
                restart ? std::optional<u16>{u16(regs.primitive_restart_index)} : std::nullopt,
                corners, reinterpret_cast<u16*>(data));
        } else {
            v.index_count = ExpandQuads<u32>(
                reinterpret_cast<const u32*>(address), count,
                restart ? std::optional<u32>{regs.primitive_restart_index} : std::nullopt,
                corners, reinterpret_cast<u32*>(data));
        }
        v.index_buffer = buffer;
        v.index_offset = offset;
        return true;
    }
    if (!is_indexed) {
        return true;
    }
    const VAddr address =
        regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size;
    const auto found =
        GuestBuffer(address, u64(regs.num_indices) * index_size, false, MirrorKind('i')); // FIX-177
    if (!found) {
        return false;
    }
    if (InPlaceSampled()) {
        in_place.Note(BoundKind(1), address, u64(regs.num_indices) * index_size); // NAT-142
    }
    v.index_buffer = found->first;
    v.index_offset = found->second;
    return true;
}

void NativeRenderer::RecordVertexIndex(const VertexIndexState& v) {
    Stream& s = stream;
    const CmdBuf cmdbuf{s.cmd};
    const bool dynamic_input = r.instance.IsVertexInputDynamicState();
    // OPT-005: what the list already has is not recorded again (vertex input state and vertex
    // and index bindings outlive pipeline binds; the list's other pipelines are compute).
    const bool known = s.vertex_state_known;
    s.vertex_state_known = true;
    if (dynamic_input) {
        if (!known || !std::ranges::equal(s.input_bindings, v.input_bindings) ||
            !std::ranges::equal(s.attributes, v.attributes)) {
            cmdbuf.setVertexInputEXT(v.input_bindings, v.attributes);
            s.input_bindings.assign(v.input_bindings.begin(), v.input_bindings.end());
            s.attributes.assign(v.attributes.begin(), v.attributes.end());
        } else {
            ++stats.vertex_skipped;
        }
    }
    if (const u32 count = static_cast<u32>(v.buffers.size()); count != 0) {
        const bool same = known && std::ranges::equal(s.vertex_buffers, v.buffers) &&
                          std::ranges::equal(s.vertex_offsets, v.offsets) &&
                          (dynamic_input || (std::ranges::equal(s.vertex_sizes, v.sizes) &&
                                             std::ranges::equal(s.vertex_strides, v.strides)));
        if (!same) {
            if (dynamic_input) {
                cmdbuf.bindVertexBuffers(0, count, v.buffers.data(), v.offsets.data());
            } else {
                cmdbuf.bindVertexBuffers2(0, count, v.buffers.data(), v.offsets.data(),
                                          v.sizes.data(), v.strides.data());
            }
            s.vertex_buffers.assign(v.buffers.begin(), v.buffers.end());
            s.vertex_offsets.assign(v.offsets.begin(), v.offsets.end());
            s.vertex_sizes.assign(v.sizes.begin(), v.sizes.end());
            s.vertex_strides.assign(v.strides.begin(), v.strides.end());
        }
    }
    if (v.index_buffer) {
        if (!known || s.index_buffer != v.index_buffer || s.index_offset != v.index_offset ||
            s.index_type != v.index_type) {
            cmdbuf.bindIndexBuffer(v.index_buffer, v.index_offset, v.index_type);
            s.index_buffer = v.index_buffer;
            s.index_offset = v.index_offset;
            s.index_type = v.index_type;
        }
    }
}

bool NativeRenderer::NeedsBarrierFor(const DrawBindings& b) const {
    const Stream& s = stream;
    if (s.buffers_written) {
        return true;
    }
    for (const auto* entry : b.sampled) {
        if (std::ranges::find(s.written, entry) != s.written.end()) {
            return true;
        }
    }
    for (const auto* entry : b.stored) {
        if (std::ranges::find(s.written, entry) != s.written.end()) {
            return true;
        }
    }
    return false;
}

void NativeRenderer::Written(const Native::ImageEntry* entry, u64 at) {
    if (!entry) {
        return;
    }
    auto* written = const_cast<Native::ImageEntry*>(entry);
    const bool watched = Native::WatchedTexture(entry->info.guest_address,
                                                entry->info.guest_address + 1);
    const bool first_write = watched && written->write_version.load(std::memory_order_relaxed) == 0;
    written->write_version.store(at != 0 ? at : ++write_clock, std::memory_order_relaxed);
    if (watched) {
        // FIX-159 (diagnosis, RYGT_FRAME_DUMP_ON_STORE=<seconds>): from that long after start,
        // the first write of a watched picture starts a frame dump there (the frames a
        // time-triggered dump never lands on).
        static const double on_store = [] {
            const char* env = std::getenv("RYGT_FRAME_DUMP_ON_STORE");
            return env && *env ? std::atof(env) : -1.0;
        }();
        static const auto started = std::chrono::steady_clock::now();
        static bool dumped = false;
        if (on_store >= 0 && !dumped &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >=
                on_store) {
            dumped = true;
            Native::g_dump_request = true;
        }
        if (first_write || Native::WatchSample()) {
            LOG_WARNING(Render_Vulkan, "FIX-157 watch: {} {:#x} {}x{} layers {} written, version {} "
                        "(frame {})", entry->from_cpu ? "texture" : "target",
                        entry->info.guest_address, entry->info.size.width,
                        entry->info.size.height, entry->info.resources.layers,
                        written->write_version.load(), frame);
        }
    }
    // NAT-111: the textures made of it copy its picture in before their next read.
    std::scoped_lock lock{written->sources_mutex};
    for (auto* texture : written->dependents) {
        texture->sources_dirty.store(true, std::memory_order_relaxed);
    }
}

void NativeRenderer::RefreshSources(Native::ImageEntry& texture) {
    // NAT-111: only when one of its targets was drawn since the last time.
    if (!texture.sources_dirty.exchange(false, std::memory_order_relaxed)) {
        return;
    }
    static thread_local std::vector<Native::SourceLink> sources;
    {
        std::scoped_lock lock{texture.sources_mutex};
        if (texture.sources.empty()) {
            return;
        }
        sources.assign(texture.sources.begin(), texture.sources.end());
    }
    // Per subresource of the texture, the newest picture drawn for it (several layouts take
    // turns in the same memory), when newer than what the texture holds.
    struct Pick {
        Native::ImageEntry* source;
        Native::SourcePlace place;
        u64 version;
    };
    boost::container::small_vector<Pick, 16> picks;
    auto& held = held_versions[&texture];
    const u64 own = texture.write_version.load(std::memory_order_relaxed);
    for (const auto& [source, place] : sources) {
        const u64 version = source->write_version.load(std::memory_order_relaxed);
        if (version == 0 || !source->image || source->reused.load(std::memory_order_relaxed)) {
            continue; // FIX-157: a reused one is stale
        }
        const u32 source_layers = std::max(source->image->Desc().layers, 1u);
        if (place.src_layer < 0 || place.layer < 0 ||
            u32(place.src_layer) >= source_layers ||
            u32(place.layer) >= std::max(texture.image->Desc().layers, 1u)) {
            continue;
        }
        const u32 layers = std::min(source_layers - u32(place.src_layer),
                                    std::max(texture.image->Desc().layers, 1u) - u32(place.layer));
        // A layered target and individual faces can overlap. Choose and remember the newest
        // source for each face; choosing only the first face lets an older array overwrite a
        // newer face later in the copy list (or after that face was already copied).
        for (u32 layer = 0; layer < layers; ++layer) {
            auto face = place;
            face.layer += s32(layer);
            face.src_layer += s32(layer);
            const u32 key = (u32(face.mip) << 16) | u32(face.layer);
            if (version <= std::max(held[key], own)) {
                continue;
            }
            const auto same = std::ranges::find_if(picks, [&](const Pick& pick) {
                return pick.place.mip == face.mip && pick.place.layer == face.layer;
            });
            if (same == picks.end()) {
                picks.push_back({source, face, version});
            } else if (version > same->version) {
                *same = {source, face, version};
            }
        }
    }
    if (picks.empty()) {
        return;
    }
    Stream& s = Begin();
    for (const auto& pick : picks) {
        ApplyRewrite(*pick.source); // NAT-122
    }
    PrepareImages();  // the texture's own upload goes first
    GlobalBarrier();  // the sources' pictures are written
    const vk::Image dst{Handle<VkImage_T>(texture.image->NativeHandle())};
    std::ranges::sort(picks, {}, [](const Pick& pick) {
        return std::pair{pick.place.mip, pick.place.layer};
    });
    for (size_t index = 0; index < picks.size();) {
        const auto& pick = picks[index];
        const auto& place = pick.place;
        const u32 width = std::min(
            {place.width, std::max(texture.info.size.width >> place.mip, 1u),
             std::max(pick.source->info.size.width >> place.src_mip, 1u)});
        const u32 height = std::min(
            {place.height, std::max(texture.info.size.height >> place.mip, 1u),
             std::max(pick.source->info.size.height >> place.src_mip, 1u)});
        // Keep contiguous faces from the same source in one GPU copy, without crossing a
        // face selected from a newer source.
        u32 layers = 1;
        while (index + layers < picks.size()) {
            const auto& next = picks[index + layers];
            if (next.source != pick.source || next.version != pick.version ||
                next.place.mip != place.mip || next.place.src_mip != place.src_mip ||
                next.place.layer != place.layer + s32(layers) ||
                next.place.src_layer != place.src_layer + s32(layers) ||
                next.place.width != place.width || next.place.height != place.height) {
                break;
            }
            ++layers;
        }
        const vk::ImageCopy region{
            .srcSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor,
                            .mipLevel = u32(place.src_mip),
                            .baseArrayLayer = u32(place.src_layer),
                            .layerCount = layers},
            .srcOffset = {0, 0, 0},
            .dstSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor,
                            .mipLevel = u32(place.mip),
                            .baseArrayLayer = u32(place.layer),
                            .layerCount = layers},
            .dstOffset = {0, 0, 0},
            .extent = {width, height, 1},
        };
        CmdBuf{s.cmd}.copyImage(vk::Image{Handle<VkImage_T>(pick.source->image->NativeHandle())},
                                vk::ImageLayout::eGeneral, dst, vk::ImageLayout::eGeneral, region);
        if (Rasterizer::Traced(texture.info.guest_address, texture.info.guest_size)) {
            LOG_WARNING(Render_Vulkan,
                        "NAT-130: texture {:#x} {}x{} mips {} layers {} <- target {:#x} {}x{} "
                        "layers {} at mip {} layer {} ({} layers, version {})",
                        texture.info.guest_address, texture.info.size.width,
                        texture.info.size.height, texture.info.resources.levels,
                        texture.info.resources.layers, pick.source->info.guest_address,
                        pick.source->info.size.width, pick.source->info.size.height,
                        pick.source->info.resources.layers, place.mip, place.layer, layers,
                        pick.version);
        }
        for (u32 layer = 0; layer < layers; ++layer) {
            held[(u32(place.mip) << 16) | (u32(place.layer) + layer)] = pick.version;
        }
        index += layers;
    }
    s.written.push_back(&texture); // read after the next barrier
    stats.source_copies += picks.size();
    ++s.commands;
}

void NativeRenderer::NoteWrites(const Targets* targets, const DrawBindings& b) {
    Stream& s = stream;
    const auto note = [&](const Native::ImageEntry* entry) {
        Written(entry);
        if (entry && r.FrameDumpCollecting() &&
            std::ranges::find(dump_entries, entry) == dump_entries.end()) {
            dump_entries.push_back(entry); // NAT-116
        }
        if (entry && std::ranges::find(s.written, entry) == s.written.end()) {
            s.written.push_back(entry);
        }
    };
    for (const auto* entry : b.sampled) {
        if (entry && std::ranges::find(s.read, entry) == s.read.end()) {
            s.read.push_back(entry); // NAT-140
        }
    }
    if (targets) {
        const auto attached = [&](const Native::ImageEntry* entry) {
            if (entry && std::ranges::find(s.attach_dirty, entry) == s.attach_dirty.end()) {
                s.attach_dirty.push_back(entry); // NAT-140
            }
        };
        for (const auto* entry : targets->colors) {
            attached(entry);
        }
        attached(targets->depth);
        for (const auto* entry : targets->colors) {
            note(entry);
            // NAT-112: small pictures go back to guest memory.
            constexpr u64 WritebackSize = 64_KB;
            if (entry && entry->info.guest_size <= WritebackSize && entry->info.num_samples == 1 &&
                std::ranges::find(writebacks, entry) == writebacks.end()) {
                writebacks.push_back(const_cast<Native::ImageEntry*>(entry));
            }
        }
        note(targets->depth);
    }
    for (const auto* entry : b.stored) {
        note(entry);
        constexpr u64 WritebackSize = 64_KB; // NAT-112, as for render targets
        if (entry->info.guest_size <= WritebackSize && entry->info.num_samples == 1 &&
            !entry->info.props.is_depth &&
            std::ranges::find(writebacks, entry) == writebacks.end()) {
            writebacks.push_back(const_cast<Native::ImageEntry*>(entry));
        }
    }
    s.buffers_written |= b.writes_buffers;
    // NAT-119: images the shader overwrote in memory are uploaded again (after this work, in
    // GPU order: the next PrepareImages records the uploads behind a barrier).
    for (const auto& [address, size] : b.gpu_writes) {
        if (b.fill_pattern && address == b.fill_address) {
            GpuWrote(address, std::min(size, b.fill_size), &*b.fill_pattern, true); // NAT-120
        } else {
            GpuWrote(address, size, nullptr, false); // NAT-129: what the shader may write
        }
    }
}

// DIAG-208: purple pixels of the garage cockpit view, and what each draw left there.
namespace {
constexpr VAddr TracedTarget = 0x1005000000;
constexpr std::array<std::pair<s32, s32>, 4> TracedPixels{
    {{266, 190}, {538, 306}, {273, 195}, {571, 322}}};
constexpr u64 PixelTraceDraws = 8192;
struct PixelTrace {
    std::unique_ptr<Native::Rhi::Buffer> buffer;
    std::vector<std::pair<u64, u64>> hashes;
} g_pixel_trace;
} // namespace

void NativeRenderer::Draw(bool is_indexed, u32 index_offset) {
    std::scoped_lock lock{mutex};
    if (ParallelRecordingRequested() && !parallel_sidecar_init_failed.load(std::memory_order_relaxed) && RytyGtsInRace() && !r.instance.IsIntel() && !FindParallelRuntime(this)) {
        try {
            auto sidecar=std::make_unique<ParallelRuntime>();
            sidecar->ahead_context=FindAheadBlocking(this);
            if(PreparedSampledProofRequested()) {
                sidecar->prepared_proof_context=prepared_proof_contexts.FindBlocking(this);
            }
            if(const char* path=std::getenv("RYGT_PARALLEL_MODE_FILE");path && *path) {
                const char* initial=std::getenv("RYGT_PARALLEL_INITIAL_MODE");
                const bool enabled=Native::Parallel::InitialMode(path,initial);
                sidecar->applied_mode=Native::Parallel::AppliedMode(enabled);
                sidecar->mode_controller=std::make_unique<Native::Parallel::ModeController>(std::filesystem::path(path),enabled);
            }
            if(const char* path=std::getenv("RYGT_PARALLEL_VIEW_MODE_FILE");path && *path)
                sidecar->view_mode_controller=std::make_unique<Native::Parallel::ModeController>(std::filesystem::path(path),sidecar->view_applied_mode.Enabled());
            sidecar->coordinator=std::make_unique<Native::Parallel::PassCoordinator>(*device,r.instance,*timeline,2,32,64*1024*1024,64);
            if(const char* path=std::getenv("RYGT_PARALLEL_TRANSFER_MODE_FILE");path && *path)
                sidecar->transfer_mode_controller=std::make_unique<Native::Parallel::ModeController>(std::filesystem::path(path),sidecar->transfer_applied_mode.Enabled());
            std::lock_guard sidecar_lock(parallel_sidecars_mutex);
            auto [where,inserted]=parallel_sidecars.emplace(this,std::move(sidecar));
            if(inserted && where->second->prepared_proof_context)where->second->prepared_proof_context->SetMode(where->second->applied_mode.Enabled());
            if(inserted && where->second->ahead_context) {
                std::scoped_lock ahead_lock{ahead_context_mutex};
                if(!where->second->ahead_context->closing.load(std::memory_order_acquire))
                    SetAheadMode(*where->second->ahead_context,where->second->applied_mode.Enabled());
            }
            parallel_sidecars_generation.fetch_add(1,std::memory_order_release);
        } catch(const std::exception&) {parallel_sidecar_init_failed.store(true,std::memory_order_relaxed);LOG_WARNING(Render_Vulkan,"PARALLEL-RECORD: initialization failed, using serial recorder");}
    }
    const auto start = std::chrono::steady_clock::now();
    t_timed = r.FrameDumpCollecting() || (++timing_tick & 15) == 0; // F8: every draw
    SceneTrace::CpuScope scene_trace{r.FrameDumpCollecting(),frame,"draw",stats.phase_ns,stats.record_ns};
    if (SceneTrace::Id()) r.FrameDumpEvent(fmt::format("scene-pass id={} kind=draw indexed={} index_offset={}", SceneTrace::Id(),is_indexed,index_offset));
    stats.timed_draws += t_timed ? 1 : 0;
    struct Timed {
        std::chrono::steady_clock::time_point start;
        Stats& stats;
        bool sampled;
        std::array<u64, 8> phase_before{};
        std::array<u64, 8> record_before{};
        Timed(std::chrono::steady_clock::time_point start_, Stats& stats_)
            : start{start_}, stats{stats_}, sampled{t_timed} {
            if (sampled) {
                phase_before = stats.phase_ns;
                record_before = stats.record_ns;
            }
        }
        ~Timed() {
            const u64 elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count();
            stats.draw_ns += elapsed;
            if (!sampled || elapsed < 1'000'000) {
                return;
            }
            static thread_local auto next_report = std::chrono::steady_clock::time_point{};
            const auto now = std::chrono::steady_clock::now();
            if (now < next_report) {
                return;
            }
            next_report = now + std::chrono::milliseconds{100};
            // Report() may reset the interval counters at the end of this draw.
            for (size_t i = 0; i < phase_before.size(); ++i) {
                if (stats.phase_ns[i] < phase_before[i] ||
                    stats.record_ns[i] < record_before[i]) {
                    return;
                }
            }
            std::array<double, 8> phases{}, parts{};
            for (size_t i = 0; i < phases.size(); ++i) {
                phases[i] = (stats.phase_ns[i] - phase_before[i]) / 1e6;
                parts[i] = (stats.record_ns[i] - record_before[i]) / 1e6;
            }
            LOG_WARNING(Render_Vulkan,
                "NAT: slow sampled draw {:.2f} ms; pipeline/targets/buffers/textures/sources/vertices/prepare/record "
                "{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}; "
                "record barrier/begin/dynamic/vertex/pipeline/bindings/draw/notes "
                "{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}",
                elapsed / 1e6, phases[0], phases[1], phases[2], phases[3], phases[4],
                phases[5], phases[6], phases[7], parts[0], parts[1], parts[2], parts[3],
                parts[4], parts[5], parts[6], parts[7]);
        }
    } timed{start, stats};
    r.scheduler.PopPendingOperations();
    const auto& regs = r.liverpool->DrawRegs();
    switch (Classify(regs)) {
    case Filter::EliminateFastClear: {
        // Fast-cleared tiles get the clear colour now when nothing drew into the target since
        // its CMASK was cleared (otherwise the pass that drew started with the clear).
        const auto& col_buf = regs.color_buffers[0];
        if (UiHires::Enabled() && UiHires::KnownAddress(col_buf.Address())) {
            ResolveUiDisplay(col_buf.Address(), false);
        }
        if (col_buf && col_buf.info.fast_clear && cleared_meta.erase(col_buf.CmaskAddress())) {
            const VideoCore::TextureCache::ImageDesc desc{col_buf, r.liverpool->DrawCbExtent(0)};
            if (Native::ImageEntry* entry = registry->Find(desc.info, true, frame)) {
                Stream& s = Begin();
                ApplyRewrite(*entry); // NAT-122
                PrepareImages();
                GlobalBarrier();
                const vk::ImageSubresourceRange range{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = desc.view_info.range.base.layer,
                    .layerCount = desc.view_info.range.extent.layers,
                };
                CmdBuf{s.cmd}.clearColorImage(
                    vk::Image{Handle<VkImage_T>(entry->image->NativeHandle())},
                    vk::ImageLayout::eGeneral, LiverpoolToVK::ColorBufferClearValue(col_buf).color,
                    range);
                Written(entry);
                s.buffers_written = true; // the next work waits for the clear
                ++s.commands;
            }
        }
        ++stats.skipped;
        return;
    }
    case Filter::FmaskDecompress:
    case Filter::NoPrimitive:
        ++stats.skipped;
        return;
    case Filter::Resolve:
        ResolveDraw(regs);
        return;
    case Filter::Copy: {
        static std::atomic<u32> logged{};
        if (Sample(logged)) {
            LOG_WARNING(Render_Vulkan, "NAT-102: depth/stencil copy draw skipped");
        }
        ++stats.skipped;
        return;
    }
    case Filter::Draw:
        break;
    }

    const bool quad_list = regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
    bool quad_triangles = quad_list && Common::PerfFeatureEnabled(28) &&
                          u64(regs.num_indices) / 4 * 6 * sizeof(u32) <= MaxQuadIndexBytes;
    // OPT-010: the draw a game job thread prepared, if any.
    static const bool prepare = PrepareEnabled();
    PreparedDraw* prepared = nullptr;
    if (prepare && !r.FrameDumpCollecting() && RytyGtsInRace()) {
        prepared = TakePrepared(Rasterizer::t_draw_packet);
        stats.prepared_missing += prepared ? 0 : 1;
    }
    struct GiveBack {
        NativeRenderer& n;
        PreparedDraw* p;
        ~GiveBack() {
            if (p) {
                n.FreePrepared(p);
            }
        }
    } give_back{*this, prepared};
    // OPT-012: when the command thread didn't select (it saw the prepared draw), the job
    // thread's selection is installed and trusted.
    bool trusted = false;
    const GraphicsPipeline* pipeline = [&]() -> const GraphicsPipeline* {
        PhaseClock clock{stats.phase_ns[0]};
        if (prepared && !quad_list && r.selected_pipeline == nullptr && prepared->pipeline) {
            if (const GraphicsPipeline* installed = InstallPrepared(*prepared)) {
                trusted = true;
                return installed;
            }
        }
        return r.AcquireGraphicsPipeline(
            {
                .vertex_sgpr_offset = 0,
                .instance_sgpr_offset = 0,
                .tessellate_quads = quad_list && !quad_triangles,
            },
            !quad_list);
    }();
    if (!pipeline) {
        ++stats.no_pipeline;
        return;
    }
    // DIAG-152 (RYGT_NATIVE_SKIP_PS=<hex,...>): draws with these pixel shaders are skipped.
    static const std::vector<u64> skip_ps = [] {
        std::vector<u64> hashes;
        if (const char* env = std::getenv("RYGT_NATIVE_SKIP_PS"); env && *env) {
            const std::string list{env};
            for (size_t start = 0; start < list.size();) {
                const size_t end = std::min(list.find(',', start), list.size());
                hashes.push_back(std::stoull(list.substr(start, end - start), nullptr, 16));
                start = end + 1;
            }
        }
        return hashes;
    }();
    if (!skip_ps.empty()) {
        const auto& stages = pipeline->GetStages();
        const Shader::Info* fs = stages[u32(Shader::SwStage::Fragment)];
        if (fs && std::ranges::find(skip_ps, fs->pgm_hash) != skip_ps.end()) {
            ++stats.skipped;
            static std::atomic<u32> logged{};
            if (logged++ < 32) {
                LOG_WARNING(Render_Vulkan, "DIAG-152: draw with ps {:#x} skipped", fs->pgm_hash);
            }
            return;
        }
    }
    if (RytyGhost::ShouldSmooth(regs) && ++smooth_ghost_draws == 1) {
        const auto& appearance = RytyGhost::GetAppearance();
        LOG_INFO(Render_Vulkan,
                 "Native ghost smoothing: body {}%, rubber {}%, rim {}%, rim brightness {}%",
                 appearance.body_opacity, appearance.rubber_opacity, appearance.rim_opacity,
                 appearance.rim_brightness);
    }
    SyncUiReads(pipeline->GetStages());
    Stream& s = Begin();
    Targets targets;
    {
        PhaseClock clock{stats.phase_ns[1]};
        ResolveTargets(regs, *pipeline, targets);
    }

    // Experimental post-process resolution: only the known full-screen composition draw.
    static const bool low_composition = [] {
        const char* env = std::getenv("RYGT_LOW_RES_COMPOSITION");
        return env && env[0] == '1';
    }();
    const auto* composition_fs = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    bool scale_composition = false;
    if (low_composition && composition_fs && composition_fs->pgm_hash == 0xb33ec4df &&
        targets.colors[0] && !targets.depth && !regs.blend_control[0].enable && regs.color_target_mask.GetMask(0) == 0xf &&
        std::none_of(targets.colors.begin() + 1, targets.colors.end(), [](auto* e) { return e != nullptr; })) {
        const auto& desc = targets.colors[0]->image->Desc();
        const auto flags = r.instance.GetPhysicalDevice().getFormatProperties(
            targets.color_formats[0]).optimalTilingFeatures;
        const auto required = vk::FormatFeatureFlagBits::eBlitSrc |
                              vk::FormatFeatureFlagBits::eBlitDst |
                              vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
        if (desc.width == 1920 && desc.height == 1080 && desc.samples == 1 &&
            desc.layers == 1 && desc.mips == 1 && u32(desc.format) == u32(targets.color_formats[0]) &&
            (flags & required) == required) {
            if (!composition_scratch) {
                auto scratch_desc = desc;
                scratch_desc.width = 960;
                scratch_desc.height = 540;
                scratch_desc.usage = Native::Rhi::ImageUsageTransferSrc |
                                     Native::Rhi::ImageUsageTransferDst;
                scratch_desc.name = "composition scale scratch";
                composition_scratch = device->CreateImage(scratch_desc);
            }
            scale_composition = composition_scratch &&
                composition_scratch->Desc().format == desc.format;
        }
    }

    if (r.FrameDumpActive() || Rasterizer::ExposureLogOn()) {
        r.FrameDumpPass(regs, targets.state, pipeline); // NAT-117
    }
    // OPT-010: the bindings the job thread worked out ahead, when they hold.
    bool from_prepared = false;
    if (prepared) {
        PhaseClock clock{stats.phase_ns[2]};
        from_prepared = CommitPrepared(*prepared, *pipeline, regs, trusted);
    }
    bool prepared_vertices_valid=false;
    if(from_prepared && prepared->has_vertices && !prepared->bindings.failed) {
        PhaseClock clock{stats.phase_ns[5]};
        prepared_vertices_valid=CommitPreparedVertices(*prepared,*pipeline,regs,is_indexed,index_offset,quad_triangles,trusted);
        // MirrorBuffer may process CPU writes after CommitPrepared. A changed
        // range invalidates both prepared bindings and vertices, not only vi.
        if(prepared->unmap_gen!=unmap_generation.load(std::memory_order_acquire) ||
           !buffer_log.Untouched(prepared->buffer_seq,prepared->buffer_ranges)) {
            from_prepared=false;prepared_vertices_valid=false;
        }
    }
    DrawBindings& b = from_prepared ? prepared->bindings : bindings;
    if (!from_prepared) {
        b.Clear();
        b.push = UserData(regs);
        BindStages(pipeline->GetStages(), regs, false, b);
    }
    if (b.failed) {
        ++stats.skipped;
        return;
    }
    static thread_local VertexIndexState vertex_index;
    const VertexIndexState* vi = &vertex_index;
    {
        PhaseClock clock{stats.phase_ns[5]};
        if (from_prepared && prepared_vertices_valid) {
            vi = prepared->vertex_index.get(); // OPT-011
        } else {
            vertex_index = {};
            ResolveVertexBuffers(*pipeline, regs, vertex_index);
            if (!ResolveIndices(regs, is_indexed, index_offset, quad_triangles, vertex_index)) {
                ++stats.skipped;
                return;
            }
            CheckVertexReach(*pipeline, regs, is_indexed, index_offset); // FIX-181 (diagnosis)
        }
    }
    {
        PhaseClock clock{stats.phase_ns[6]};
        PrepareImages();
    }
    const bool native_fsr_composition = BeginNativeFsr(*pipeline, targets, b);
    if (native_fsr_composition) scale_composition = false;
    if (!native_fsr_composition && !PrepareUiTarget(targets, *pipeline)) {
        ++stats.skipped;
        return;
    }
    Begin();
    // A color image sampled while this draw writes it is a feedback attachment. Native
    // images normally stay in GENERAL, but that does not make an overlapping attachment
    // read legal. Enter the feedback layout for this draw and restore GENERAL afterward.
    boost::container::small_vector<const Native::ImageEntry*, 8> feedback;
    if (r.instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        for (u32 cb = 0; cb < targets.colors.size(); ++cb) {
            const auto* target = targets.colors[cb];
            if (target && std::ranges::find(b.sampled, target) != b.sampled.end() &&
                std::ranges::find(feedback, target) == feedback.end()) {
                // Output-resolution UI can redirect this attachment to a separate proxy.
                // Only the original image's own attachment views form a feedback loop.
                std::scoped_lock lock{views_mutex};
                const auto found = views.find(target);
                if (found != views.end() && std::ranges::any_of(found->second, [&](const auto& view) {
                        return view.use == ViewUse::Attachment &&
                               *view.view == targets.state.color_attachments[cb].image_view;
                    })) {
                    feedback.push_back(target);
                }
            }
        }
    }
    const auto feedback_layout = [&](vk::ImageLayout from, vk::ImageLayout to) {
        boost::container::static_vector<vk::ImageMemoryBarrier2, 8> barriers;
        for (const auto* entry : feedback) {
            barriers.push_back(vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .oldLayout = from,
                .newLayout = to,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = vk::Image{Handle<VkImage_T>(entry->image->NativeHandle())},
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            });
        }
        s.cmd.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
            .pImageMemoryBarriers = barriers.data(),
        });
    };
    if (!feedback.empty()) {
        GlobalBarrier();
        feedback_layout(vk::ImageLayout::eGeneral,
                        vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT);
        for (u32 cb = 0; cb < targets.colors.size(); ++cb) {
            if (std::ranges::find(feedback, targets.colors[cb]) != feedback.end()) {
                targets.state.color_attachments[cb].image_layout =
                    vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT;
            }
        }
        std::scoped_lock lock{views_mutex};
        for (const auto* entry : feedback) {
            const auto found = views.find(entry);
            if (found == views.end()) {
                continue;
            }
            for (const auto& view : found->second) {
                if (view.use != ViewUse::Sampled && view.use != ViewUse::CubeSampled) {
                    continue;
                }
                for (auto& image : b.images) {
                    if (image.imageView == *view.view) {
                        image.imageLayout = vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT;
                    }
                }
            }
        }
    }
    PhaseClock record_clock{stats.phase_ns[7]};
    {
        PhaseClock part{stats.record_ns[0]}; // OPT-011
        // NAT-140: a full barrier only when the draw reads what was written since the last
        // one, or draws into what was read since; a pass that draws into pictures earlier
        // passes drew waits for their attachment writes only; other passes don't wait.
        static const bool pass_barriers = [] {
            const char* env = std::getenv("RYGT_NATIVE_PASS_BARRIERS");
            return !(env && env[0] == '0');
        }();
        const bool pass_change = !s.rendering || !(s.render == targets.state);
        // FIX-183 (diagnosis, RYGT_NATIVE_DRAW_BARRIER=1): a full barrier before every draw.
        static const bool draw_barrier = [] {
            const char* env = std::getenv("RYGT_NATIVE_DRAW_BARRIER");
            return env && env[0] == '1';
        }();
        if (draw_barrier) {
            GlobalBarrier();
        }
        const auto targets_in = [&](const std::vector<const Native::ImageEntry*>& list) {
            if (list.empty()) {
                return false;
            }
            for (const auto* entry : targets.colors) {
                if (entry && std::ranges::find(list, entry) != list.end()) {
                    return true;
                }
            }
            return targets.depth && std::ranges::find(list, targets.depth) != list.end();
        };
        if (!pass_barriers) {
            if (pass_change || NeedsBarrierFor(b)) {
                GlobalBarrier();
            }
        } else if (NeedsBarrierFor(b) || (pass_change && targets_in(s.read))) {
            GlobalBarrier();
        } else if (pass_change && targets_in(s.attach_dirty)) {
            PassBarrier();
        } else if (pass_change) {
            ++stats.passes_unblocked;
        }
    }
    auto* parallel=FindParallelRuntime(this);
    const auto partition_mode=parallel && parallel->applied_mode.Enabled()?1u:0u;
    RecordPartitionEndGuard end_partition_guard{parallel?&parallel->record_partition[partition_mode][4]:nullptr};
    RecordPartitionClock admission_partition{parallel?&parallel->record_partition[partition_mode][0]:nullptr};
    // Narrow upper bound only; no draw eligibility is enabled. Unknown query,
    // alias and lifetime constraints are not established by these counters.
    if (ParallelEligibilityEnabled()) {
        if (parallel_eligibility_owner != this) { parallel_eligibility_stats={}; parallel_eligibility_owner=this; }
        auto& probe=parallel_eligibility_stats;
        if (!s.rendering || !(s.render==targets.state)) probe.Flush();
        ++probe.observed;
        const auto& depth=targets.state.depth_stencil_attachment;
        const bool in_race=RytyGtsInRace();
        probe.race+=in_race; probe.nonindexed+=!quad_list && !is_indexed;
        probe.single_color+=targets.state.num_color_attachments==1;
        probe.no_clear+=targets.state.num_color_attachments==1 && !targets.state.color_attachments[0].is_clear;
        probe.no_depth+=!depth.has_depth && !depth.has_stencil; probe.no_descriptors+=b.writes.empty();
        probe.no_storage+=!b.writes_buffers && b.stored.empty(); probe.no_feedback+=feedback.empty();
        const bool eligible=in_race && !r.FrameDumpActive() && !quad_list && !is_indexed &&
            targets.state.num_color_attachments==1 && !targets.state.color_attachments[0].is_clear &&
            !depth.has_depth && !depth.has_stencil && b.writes.empty() && !b.writes_buffers &&
            b.stored.empty() && feedback.empty();
        if (eligible) {++probe.eligible;++probe.current;} else probe.Flush();
    }
    // Experimental first subset: a complete, descriptor-free, vertexless color
    // scope. Every eligibility uncertainty falls back to the existing recorder.
    // Declared after GiveBack: an uncommitted draw flushes before prepared owners
    // and the renderer mutex are released, including during exception unwinding.
    struct PendingDrawGuard {
        PendingDescriptorUpdates* pending;vk::Device device;bool committed{};
        ~PendingDrawGuard() noexcept {if(pending && !committed)pending->Flush(device,PendingDescriptorUpdates::Abandoned);}
    } pending_draw_guard{parallel?&parallel->descriptor_updates:nullptr,r.instance.GetDevice()};
    static const bool natural_boundary_enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_NATURAL_BOUNDARY");return p && p[0]=='1' && !p[1];}();
    bool preclosed_serial_scope=false;
    bool serial_split_started=false;
    bool serial_split_prebound=false;
    const auto cancel_empty_split=[&]() noexcept {
        if(!serial_split_started || !parallel || !parallel->active || !parallel->active->draws.empty())return;
        parallel->descriptor_updates.Flush(r.instance.GetDevice(),PendingDescriptorUpdates::Abandoned);
        if(!parallel->sync_pins.CancelEmpty(parallel->pin_slot))std::terminate();
        parallel->pin_slot=Native::Parallel::SyncOwnerPins<>::Invalid;
        parallel->scratch_draws.swap(parallel->active->draws);
        parallel->active.reset();parallel->lease.reset();s.rendering=false;
        s.dynamic.Invalidate();s.pipelines.Invalidate();s.vertex_state_known=false;
    };
    struct EmptySplitGuard {
        const decltype(cancel_empty_split)& cancel;
        ~EmptySplitGuard() noexcept {cancel();}
    } empty_split_guard{cancel_empty_split};
    if(parallel && !parallel->closing && parallel->applied_mode.Enabled() && natural_boundary_enabled) {
        preclosed_serial_scope=Native::Parallel::CloseChangedSerialScope(true,bool(parallel->active),s.rendering,s.render,targets.state,[&]{EndRendering();});
        if(preclosed_serial_scope)++parallel->natural_preclosed;
    }
    bool parallel_draw=false;
    bool parallel_index_safe=false;u64 parallel_index_bytes=0;
    ParallelBindingCapture parallel_bindings{};
    std::shared_ptr<const DescriptorSnapshot> parallel_push_writes;
    std::size_t parallel_descriptor_bytes=0;
    u32 parallel_required_sets=0;
    auto* worker_result = parallel && prepared && from_prepared && prepared->pipeline==pipeline ? &prepared->cpu_proof : nullptr;
    const bool worker_identity = worker_result && worker_result->prepared_identity==prepared &&
        worker_result->pipeline_identity==pipeline && worker_result->pipeline_layout==pipeline->GetLayout() &&
        worker_result->writes_data==b.writes.data() && worker_result->writes_size==b.writes.size() &&
        worker_result->buffers_data==b.buffers.data() && worker_result->buffers_size==b.buffers.size() &&
        worker_result->images_data==b.images.data() && worker_result->images_size==b.images.size();
    // Producer totals cover every ordinary row, including descriptor-cache hits.
    // Match each bounded group span before using them with the live structure proof.
    // Like known_structure, this relies on published row metadata remaining immutable.
    const auto set_count_proof=[&] {
        if(!worker_identity || !worker_result->descriptor_structure || !worker_result->counts_valid ||
           !b.groups_ready || worker_result->counts_push!=pipeline->PushSet())return;
        for(size_t i=0;i<b.grouped_writes.size();++i)
            if(worker_result->group_data[i]!=b.grouped_writes[i].data() || worker_result->group_sizes[i]!=b.grouped_writes[i].size())return;
        parallel_bindings.counts_proven=true;
        parallel_bindings.ordinary_writes=worker_result->ordinary_writes;
        parallel_bindings.ordinary_buffers=worker_result->ordinary_buffers;
        parallel_bindings.ordinary_images=worker_result->ordinary_images;
    };
    if(parallel && !parallel->closing && parallel->applied_mode.Enabled()) {
        static const bool whole_enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_WHOLE_PREFLIGHT");return p && p[0]=='1' && p[1]=='\0';}();
        const bool whole_sample=whole_enabled && ((++parallel->whole_seq & 127u)==0);
        u64 whole_cpu_begin=0,whole_cpu_end=0;
        const bool whole_cpu_started=whole_sample && DescriptorThreadCpu100ns(whole_cpu_begin);
        const auto whole_begin=whole_sample?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        ++parallel->considered;
        const auto& fast_depth=targets.state.depth_stencil_attachment;
        const bool extended=ParallelExtendedEnabled();
        bool any_clear=targets.state.color_attachments[0].is_clear;
        if(extended) {
            any_clear=fast_depth.depth_clear || fast_depth.stencil_clear;
            for(u32 i=0;i<targets.state.num_color_attachments && i<targets.state.color_attachments.size();++i)any_clear|=targets.state.color_attachments[i].is_clear;
        }
        const bool prohibited_clear=any_clear;
        const bool fast_race=RytyGtsInRace(),fast_patch=EmulatorSettings.IsPatchShaders();
        const bool fast_trace=r.FrameDumpActive() || SceneTrace::Id() || Native::GpuCheckpoints::Requested();
        static const bool serial_audit=[] {const char* p=std::getenv("RYGT_PARALLEL_SERIAL_AUDIT");return p && p[0]=='1' && p[1]=='\0';}();
        // Split only a matching scope whose attachments need LOAD on continuation.
        bool split_clear=fast_depth.depth_clear || fast_depth.stencil_clear;
        for(u32 i=0;i<targets.state.num_color_attachments && i<targets.state.color_attachments.size();++i)
            split_clear|=targets.state.color_attachments[i].is_clear;
        const bool same_serial_scope=s.rendering && !parallel->active && s.render==targets.state;
        const bool split_candidate=same_serial_scope && !split_clear;
        const bool serial_scope_bypass=!serial_audit && s.rendering && !parallel->active && !split_candidate;
        // Do not build a captured pass when even its prefix and pass nodes
        // cannot fit. Recheck on later serial draws as retired nodes may free.
        const bool no_pass_room=!parallel->active && !parallel->coordinator->HasRoomForPass();
        bool fast_reject=no_pass_room || serial_scope_bypass || !fast_race || fast_patch || fast_trace || quad_list || quad_triangles ||
            (!extended && is_indexed) || scale_composition || native_fsr_composition ||
            (!extended && targets.state.num_color_attachments!=1) || targets.state.num_color_attachments>8 || prohibited_clear ||
            (!extended && (fast_depth.has_depth || fast_depth.has_stencil || bool(vi->index_buffer))) ||
            !feedback.empty() || !prepared || !from_prepared ||
            prepared->pipeline!=pipeline || !prepared->pipeline_keepalive ||
            b.writes_buffers || !b.gpu_writes.empty() || !b.stored.empty();
        // UI/stream preparation may retire mirrors after vertex commit. Do not
        // inspect prepared owner pointers if any prepared range changed since.
        if(!fast_reject && (prepared->unmap_gen!=unmap_generation.load(std::memory_order_acquire) ||
           !buffer_log.Untouched(prepared->buffer_seq,prepared->buffer_ranges))) fast_reject=true;
        if(fast_reject) {
            ++parallel->fast_rejected;if(no_pass_room)++parallel->no_pass_room_rejected;if(extended && any_clear)++parallel->extended_clear_rejected;if(serial_scope_bypass)++parallel->serial_scope_bypassed;
            if(ParallelGateDiagEnabled()) {
                // Bit 25 means deeper stage/view/alias/descriptor checks were not examined.
                u64 mask=u64{1}<<25;if(serial_scope_bypass)mask|=u64{1}<<26;const auto fail=[&](u32 bit,bool bad){if(bad)mask|=u64{1}<<bit;};
                fail(27,no_pass_room);fail(0,!fast_race);fail(1,fast_patch);fail(2,fast_trace);fail(4,!feedback.empty());
                fail(5,!extended && targets.state.num_color_attachments!=1);fail(6,prohibited_clear);
                fail(7,!extended && (fast_depth.has_depth || fast_depth.has_stencil));fail(8,quad_list || quad_triangles || (!extended && is_indexed));
                fail(9,scale_composition || native_fsr_composition);fail(12,!extended && bool(vi->index_buffer));
                fail(13,!prepared || !from_prepared || prepared->pipeline!=pipeline || !prepared->pipeline_keepalive);
                fail(23,b.writes_buffers || !b.gpu_writes.empty() || !b.stored.empty());parallel->gates.Add(mask,true);
            }
            parallel->FinishSerialEligibleRun();
            if(parallel->active){EndRendering();PassBarrier();}
        } else {
        const auto& db=targets.state.depth_stencil_attachment;
        // These draw-local requirements are mandatory in cheap_eligible below.
        // Avoid region/mirror scans for draws that can never be admitted.
        const bool vertex_preflight= !quad_list && !quad_triangles && (extended || !is_indexed) &&
            !scale_composition && !native_fsr_composition && (extended || targets.state.num_color_attachments==1) &&
            !prohibited_clear && (extended || (!db.has_depth && !db.has_stencil && !vi->index_buffer)) &&
            feedback.empty() && prepared && from_prepared &&
            prepared->pipeline==pipeline && prepared->pipeline_keepalive;
        const bool vertex_requested=ParallelVertexEnabled();
        const bool has_vertex_input=!vi->buffers.empty() || !vi->input_bindings.empty() || !vi->attributes.empty();
        bool vertex_safe=!has_vertex_input && vi->offsets.empty() && vi->sizes.empty() && vi->strides.empty() && (extended || !vi->index_buffer);
        bool vertex_shape_ok=false,vertex_binding_ok=true,vertex_pnext_ok=true,vertex_owner_missing=false;
        if(vertex_requested && has_vertex_input && vertex_preflight &&
           prepared->has_vertices && prepared->vertex_index.get()==vi &&
           vi->buffers.size()<=32 && vi->input_bindings.size()<=32 && vi->attributes.size()<=32 &&
           vi->buffers.size()==vi->offsets.size() && vi->buffers.size()==vi->sizes.size() &&
           vi->buffers.size()==vi->strides.size() && !vi->buffers.empty() &&
           vi->buffers.size()==vi->input_bindings.size() && vi->buffers.size()==vi->attributes.size()) {
            vertex_shape_ok=true;vertex_safe=true;
            const bool known_shape=worker_identity && worker_result->vertex_identity==vi &&
                worker_result->vertex_buffers_data==vi->buffers.data() && worker_result->vertex_buffers_size==vi->buffers.size() && worker_result->vertex_shape;
            if (known_shape) {vertex_binding_ok=worker_result->vertex_bindings;vertex_pnext_ok=worker_result->vertex_pnext;++parallel->producer_vertex_hits;}
            else {
                for(std::size_t i=0;i<vi->buffers.size();++i)
                    if(vi->input_bindings[i].binding!=i || vi->attributes[i].binding!=i)vertex_binding_ok=false;
                for(const auto& x:vi->input_bindings) if(x.pNext)vertex_pnext_ok=false;
                for(const auto& x:vi->attributes) if(x.pNext)vertex_pnext_ok=false;
            }
            vertex_safe=vertex_binding_ok && vertex_pnext_ok;
            for(std::size_t i=0;i<vi->buffers.size() && vertex_safe;++i) {
                const auto handle=vi->buffers[i];const auto offset=vi->offsets[i],size=vi->sizes[i];
                bool owned=false;
                const auto matches=[&](const auto* value){
                    if(!value || !value->buffer || !handle || !size)return false;
                    if(vk::Buffer{Handle<VkBuffer_T>(value->buffer->NativeHandle())}!=handle)return false;
                    const u64 capacity=value->buffer->Size();return offset<=capacity && size<=capacity-offset;
                };
                for(const auto& region:prepared->regions) if(matches(region.region)){owned=true;break;}
                if(!owned)for(const auto* mirror:prepared->mirrors)if(matches(mirror)){owned=true;break;}
                if(!owned && handle && size && ParallelDirectVertexEnabled() && ParallelD2Enabled() &&
                   prepared->in_place.size()<=32) {
                    auto* direct=r.buffer_cache.GetDirectMemory();
                    if(direct) for(const auto& [address,bytes]:prepared->in_place) {
                        if(bytes!=size)continue;
                        ++parallel->vertex_direct_attempts;
                        u32 parts=0;bool exact=false;
                        const bool covered=direct->TryForEachPart(address,bytes,
                            [&] {return VideoCore::ParallelDirectViewCoherent(direct,address,bytes);},
                            [&](const VideoCore::Buffer& source,u64 source_offset,u64 part_offset,u64 length) {
                                ++parts;exact=source.Handle()==handle && source_offset==offset && part_offset==0 && length==bytes;
                            });
                        if(covered && parts==1 && exact){owned=true;++parallel->vertex_direct_matches;break;}
                    }
                }
                if(!owned){vertex_owner_missing=true;vertex_safe=false;}
            }
        }
        if(vertex_requested && has_vertex_input) {
            ++parallel->vertex_candidates;
            if(vertex_preflight) {
                ++parallel->vertex_preflight_draws;
                if(!vertex_shape_ok)++parallel->vertex_bad_structure;
                else if(!vertex_binding_ok)++parallel->vertex_bad_binding;
                else if(!vertex_pnext_ok)++parallel->vertex_bad_pnext;
                else if(vertex_owner_missing)++parallel->vertex_unknown_backing;
                if(prepared && !prepared->in_place.empty())++parallel->vertex_direct_ranges;
            }
            if(vertex_safe)++parallel->vertex_backing_pass;
        }
        bool stages_safe=true,resource_key_known=true;
        for(const auto* stage:pipeline->GetStages()) if(stage && !stage->resource_usage_known) resource_key_known=false;
        if(!resource_key_known) ++parallel->unknown_resource_key;
        for(const auto* stage:pipeline->GetStages()) if(stage &&
            (!stage->fmasks.empty() || !stage->attribute_flags_known || !stage->resource_usage_known || stage->translation_failed ||
             (stage->sw_stage!=Shader::SwStage::Vertex && stage->sw_stage!=Shader::SwStage::Fragment) ||
             (stage->has_fetch_shader && !(vertex_requested && has_vertex_input && vertex_safe && stage->sw_stage==Shader::SwStage::Vertex)) || stage->uses_patches || stage->uses_dma ||
             (stage->uses_direct && !ParallelD2Enabled()) ||
             stage->has_storage_images || stage->uses_shader_clock ||
             std::ranges::any_of(stage->buffers,[](const auto& buffer){return buffer.is_written;}))) stages_safe=false;
        // Submission-window N protects only views actually tracked by the native
        // registry; the epoch token itself is not an owning image reference.
        bool native_attachment=true;
        {
            std::scoped_lock views_lock{views_mutex};
            const auto tracked=[&](const Native::ImageEntry* entry,vk::ImageView handle) {
                if(!entry || !handle)return false;
                const auto found=views.find(entry);
                return found!=views.end() && std::ranges::any_of(found->second,[&](const auto& view){return view.use==ViewUse::Attachment && *view.view==handle;});
            };
            for(u32 i=0;i<targets.state.num_color_attachments;++i) {
                const auto handle=targets.state.color_attachments[i].image_view;
                if(handle ? !tracked(targets.colors[i],handle) : bool(targets.colors[i]))native_attachment=false;
            }
            if(db.has_depth || db.has_stencil) {
                if(!tracked(targets.depth,db.image_view))native_attachment=false;
            } else if(extended && (targets.depth || db.image_view))native_attachment=false;
            if(!extended && (!targets.colors[0] || !targets.state.color_attachments[0].image_view))native_attachment=false;
        }
        std::array<Native::Parallel::GuestImageRange,9> writable_ranges{};std::size_t writable_count=0;
        if(extended) {
            for(u32 i=0;i<targets.state.num_color_attachments;++i)if(const auto* e=targets.colors[i])
                writable_ranges[writable_count++]={e->info.guest_address,e->info.guest_size};
            if(targets.depth)writable_ranges[writable_count++]={targets.depth->info.guest_address,targets.depth->info.guest_size};
        }
        bool sampled_alias=extended && !Native::Parallel::WritableRangesDisjoint(writable_ranges,writable_count);
        for(const auto* sampled:b.sampled) {
            if(!sampled || !sampled->info.guest_size || (extended && !Native::Parallel::ValidGuestImageRange({sampled->info.guest_address,sampled->info.guest_size}))){sampled_alias=true;break;}
            const auto overlaps=[&](const Native::ImageEntry* target) {
                if(!target)return false;const u64 a=sampled->info.guest_address,as=sampled->info.guest_size;
                const u64 t=target->info.guest_address,ts=target->info.guest_size;
                return !ts || (a<=t ? t-a<as : a-t<ts);
            };
            for(const auto* target:targets.colors)if(overlaps(target)){sampled_alias=true;break;}
            if(extended && overlaps(targets.depth))sampled_alias=true;
            if(sampled_alias)break;
        }
        // Match the committed index packet to the exact prepared guest interval
        // and a currently tracked backing. The worker retains handles only.
        bool index_safe=!is_indexed && !vi->index_buffer;
        u64 checked_index_bytes=0;
        if(extended && is_indexed && prepared && prepared->has_vertices && prepared->vertex_index.get()==vi &&
           prepared->indexed && prepared->index_offset==index_offset && vi->index_buffer && vi->index_count==regs.num_indices &&
           (vi->index_type==vk::IndexType::eUint16 || vi->index_type==vk::IndexType::eUint32)) {
            const u64 element=vi->index_type==vk::IndexType::eUint16?2:4;
            const u64 base=regs.index_base_address.Address<VAddr>();
            const u64 address=prepared->index_address;
            if(Native::Parallel::PreparedIndexRange(base,index_offset,regs.num_indices,element,address,vi->index_offset,checked_index_bytes)) {
                const auto matches=[&](const auto* value){
                    if(!value || !value->buffer || vk::Buffer{Handle<VkBuffer_T>(value->buffer->NativeHandle())}!=vi->index_buffer)return false;
                    return Native::Parallel::PreparedIndexBacking(address,checked_index_bytes,value->begin,value->end,vi->index_offset,value->buffer->Size());
                };
                for(const auto& use:prepared->regions)if(matches(use.region)){index_safe=true;break;}
                if(!index_safe)for(const auto* mirror:prepared->mirrors)if(matches(mirror)){index_safe=true;break;}
                if(!index_safe && ParallelDirectVertexEnabled() && ParallelD2Enabled()) {
                    auto* direct=r.buffer_cache.GetDirectMemory();
                    if(direct && std::ranges::any_of(prepared->in_place,[&](const auto& part){return part.first==address && part.second==checked_index_bytes;})) {
                        u32 parts=0;bool exact=false;
                        const bool covered=direct->TryForEachPart(address,checked_index_bytes,
                            [&]{return VideoCore::ParallelDirectViewCoherent(direct,address,checked_index_bytes);},
                            [&](const VideoCore::Buffer& source,u64 source_offset,u64 part_offset,u64 length){++parts;exact=source.Handle()==vi->index_buffer && source_offset==vi->index_offset && !part_offset && length==checked_index_bytes;});
                        index_safe=covered && parts==1 && exact;
                    }
                }
            }
        }
        if(!b.buffers.empty())++parallel->buffer_rejected;
        if(sampled_alias)++parallel->alias_rejected;
        const bool cheap_eligible=native_attachment && !sampled_alias && !EmulatorSettings.IsPatchShaders() && RytyGtsInRace() &&
            !r.FrameDumpActive() && !SceneTrace::Id() && !Native::GpuCheckpoints::Requested() &&
            !quad_list && !quad_triangles && (extended || !is_indexed) && !scale_composition && !native_fsr_composition &&
            (extended || targets.state.num_color_attachments==1) && !prohibited_clear &&
            (extended || (!db.has_depth && !db.has_stencil)) && feedback.empty() && vertex_safe && index_safe &&
            prepared && from_prepared && prepared->pipeline==pipeline && prepared->pipeline_keepalive && stages_safe;
        bool descriptor_safe=cheap_eligible && !b.writes_buffers && b.gpu_writes.empty() && b.stored.empty();
        u32 descriptor_fail_reason=0;
        const bool sample_preflight=descriptor_safe && ((++parallel->preflight_seq & 127u)==0);
        const bool phase_sample=sample_preflight && ParallelProofPhaseEnabled();
        u64 preflight_cpu_begin=0,preflight_cpu_end=0;
        const bool preflight_cpu_started=sample_preflight && DescriptorThreadCpu100ns(preflight_cpu_begin);
        const auto preflight_begin=sample_preflight?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        if(descriptor_safe && !b.writes.empty()) {
            try {
                constexpr std::size_t MaxSnapshotBytes=64*1024;
                // Inspect ordinary writes in place; only the push group is copied.
                const auto image_infos=std::span<const vk::DescriptorImageInfo>{b.images.data(),b.images.size()};
                const auto buffer_infos=std::span<const vk::DescriptorBufferInfo>{b.buffers.data(),b.buffers.size()};
                static const bool reuse_stage_proof=[] {const char* p=std::getenv("RYGT_PARALLEL_DRAW_PROOF_REUSE");return p && p[0]=='1' && !p[1];}();
                std::optional<Native::Parallel::PerDrawStageProof<Shader::Info,Shader::MaxStageTypes-1>> stage_proof;
                if(reuse_stage_proof)stage_proof.emplace();
                static const bool view_proof_enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_VIEW_PROOF_INDEX");return p && p[0]=='1' && !p[1];}();
                static const bool wanted_view_filter=[] {const char* p=std::getenv("RYGT_PARALLEL_WANTED_VIEW_FILTER");return p && p[0]=='1' && !p[1];}();
                static const bool view_sample_enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_VIEW_PROOF_SAMPLE");return p && p[0]=='1' && !p[1];}();
                const bool sample_view=view_sample_enabled && ((++parallel->view_sample.sequence & 127u)==0);
                Native::Parallel::ViewProofStats local_view_stats{};const bool gate_view=ParallelGateDiagEnabled();
                Native::Parallel::PerDrawViewProof<vk::ImageView> view_proof{sample_view?&local_view_stats:(gate_view?&parallel->view_proof_stats:nullptr),wanted_view_filter};
                const auto note_view_cardinality=[&](std::size_t n) {if(sample_view)++parallel->view_sample.cardinality[n==0?0:n==1?1:n==2?2:n<=4?3:n<=8?4:n<=16?5:n<=32?6:n<=64?7:n<=128?8:9];};
                const bool known_structure=worker_identity && worker_result->descriptor_structure;
                const bool known_actions=known_structure && prepared->descriptor_actions.size()==b.writes.size();
                const auto current_stages=pipeline->GetStages();
                bool known_stage_schema=known_actions;
                if(known_stage_schema)for(size_t si=0;si<worker_result->stages.size();++si) {
                    const auto* stage=si<current_stages.size()?current_stages[si]:nullptr;
                    const auto& schema=worker_result->stages[si];
                    if((stage && !stage->identity) || (stage ? stage->identity : 0)!=schema.identity ||
                       (stage ? stage->buffers.size() : 0)!=schema.count){known_stage_schema=false;break;}
                }
                if(known_structure)++parallel->producer_structure_hits;
                if(known_actions)++parallel->producer_action_hits;
                if(known_stage_schema)++parallel->producer_stage_schema_hits;
                struct DrawTableCache {const std::byte* data{};std::size_t size{};const void* owner{};u64 l1{},zero{};};
                std::array<DrawTableCache,Shader::MaxStageTypes-1> table_cache{};
                std::size_t action_index=0, action_upload_lookups=0;
                for(const auto& write:b.writes) {
                    const auto* action=known_actions ? &prepared->descriptor_actions[action_index] : nullptr;
                    ++action_index;
                    if(!known_structure && (write.pNext || write.pTexelBufferView)){descriptor_fail_reason=1;descriptor_safe=false;break;}
                    const bool buffer_type=action ? action->is_buffer :
                        (write.descriptorType==vk::DescriptorType::eUniformBuffer || write.descriptorType==vk::DescriptorType::eStorageBuffer);
                    if(buffer_type) {
                        SampledProofTimer phase{parallel->proof_phases[0],phase_sample};
                        if(!known_structure && (write.pImageInfo || !DescriptorSnapshot::BufferSliceValid(write.pBufferInfo,write.descriptorCount,buffer_infos))) {descriptor_fail_reason=2;descriptor_safe=false;break;}
                        for(u32 j=0;j<write.descriptorCount;++j) {
                            const auto* info=&write.pBufferInfo[j];
                            const std::size_t index=action ? action->buffer_first+j : static_cast<std::size_t>(info-b.buffers.data());
                            bool planned_stage_checked=false;const Shader::Info* planned_stage_value=nullptr;
                            const auto planned_stage=[&]() -> const Shader::Info* {
                                if(planned_stage_checked)return planned_stage_value;
                                planned_stage_checked=true;
                                if(!known_stage_schema || !action || write.descriptorCount!=1 ||
                                   action->stage_index>=current_stages.size() || action->stage_index>=worker_result->stages.size())return nullptr;
                                const auto* stage=current_stages[action->stage_index];
                                if(stage && action->stage_local<stage->buffers.size()) {++parallel->producer_stage_lookup_hits;planned_stage_value=stage;return stage;}
                                return nullptr;
                            };
                            // This is the old final clip proof reached early only for a
                            // validated current stage and the same live null/clip predicates.
                            if(ParallelReadonlyDescriptorEnabled() && write.descriptorType==vk::DescriptorType::eStorageBuffer &&
                               !info->buffer && info->offset==0 && info->range==VK_WHOLE_SIZE && !regs.clipper_control.user_clip_plane_enable) {
                                const auto fast_clip=[&]() -> bool {
                                    if(const auto* stage=planned_stage()) {
                                        const auto& desc=stage->buffers[action->stage_local];
                                        if(desc.buffer_type==Shader::BufferType::ClipPlanes && !desc.is_written) {
                                            ++parallel->descriptor_clip_nulls;++parallel->clip_fast_hits;return true;
                                        }
                                    }
                                    return false;
                                };
                                if(TimedProofInvoke(parallel->proof_phases[8],phase_sample,fast_clip))continue;
                            }
                            const auto absent_slot=[&](const Shader::Info& stage,std::size_t local,std::span<const std::byte> bytes) -> bool {
                                static const bool enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_ABSENT_GUEST");return p && p[0]=='1' && !p[1];}();
                                if(!enabled || write.descriptorType!=vk::DescriptorType::eStorageBuffer || local>=stage.buffers.size() ||
                                   stage.buffers.size()>(SIZE_MAX-16)/16 || bytes.size()!=16+stage.buffers.size()*16)return false;
                                u32 words[4]{};std::memcpy(words,bytes.data()+16+local*16,sizeof(words));
                                if(words[3]!=0)return false;
                                ++parallel->absent_mode0_attempts;
                                const auto& desc=stage.buffers[local];const auto sharp=desc.GetSharp(stage);
                                // The linked Instance requires and enables nullDescriptor before
                                // exposing its logical device; see vk_instance.cpp 394-400, 687.
                                const bool null_enabled=bool(r.instance.GetDevice());
                                const bool accepted=Native::Parallel::IsPreparedAbsentDirectSlot(bytes,stage.buffers.size(),local,
                                    desc.IsDirectEligible(),null_enabled,!info->buffer,info->offset,info->range,sharp.base_address,sharp.GetSize());
                                if(accepted)++parallel->absent_mode0_accepted;else ++parallel->absent_mode0_rejected;
                                return accepted;
                            };
                            const auto direct_slot=[&]() -> bool {
                                if(info->buffer || info->offset!=0 || info->range!=VK_WHOLE_SIZE) return false;
                                auto* direct=r.buffer_cache.GetDirectMemory();if(!direct)return false;
                                const auto stages=pipeline->GetStages();
                                if(const auto* stage=planned_stage()) {
                                    const auto& schema=worker_result->stages[action->stage_index];
                                    if(schema.table_local<stage->buffers.size() && schema.table_upload<prepared->uploads.size() &&
                                       stage->buffers[schema.table_local].buffer_type==Shader::BufferType::DirectTable &&
                                       index>=action->stage_local && index-action->stage_local<=SIZE_MAX-schema.table_local &&
                                       prepared->uploads[schema.table_upload].info==index-action->stage_local+schema.table_local) {
                                        const auto local=action->stage_local,n=stage->buffers.size();
                                        if(!stage->uses_direct || !stage->buffers[local].IsDirectEligible() || n>(SIZE_MAX-16)/16)return false;
                                        const auto expected=16+n*16;const auto& upload=prepared->uploads[schema.table_upload];
                                        auto& cached=table_cache[action->stage_index];const u64 l1=direct->L1Address(),zero=direct->ZeroPageAddress();
                                        const std::byte* data=cached.data;
                                        if(data && cached.owner==direct && cached.size==expected && cached.l1==l1 && cached.zero==zero)++parallel->direct_table_cache_hits;
                                        else {
                                            if(upload.flat_stage>=0 || upload.size!=expected || upload.offset>prepared->upload_bytes.size() ||
                                               expected>prepared->upload_bytes.size()-upload.offset)return false;
                                            data=reinterpret_cast<const std::byte*>(prepared->upload_bytes.data()+upload.offset);
                                            u32 header[4]{};std::memcpy(header,data,sizeof(header));
                                            if((u64(header[0])|(u64(header[1])<<32))!=l1 || (u64(header[2])|(u64(header[3])<<32))!=zero)return false;
                                            cached={data,expected,direct,l1,zero};
                                        }
                                        const auto bytes=std::span<const std::byte>{reinterpret_cast<const std::byte*>(data),expected};
                                        if(absent_slot(*stage,local,bytes))return true;
                                        if(!Native::Parallel::IsPreparedDirectSlot(bytes,n,local,true,true,info->offset,info->range))return false;
                                        u32 words[4]{};std::memcpy(words,data+16+local*16,sizeof(words));
                                        return VideoCore::ParallelDirectViewCoherent(direct,u64(words[0])|(u64(words[1])<<32),words[2]);
                                    }
                                }
                                if(reuse_stage_proof) {
                                    const auto* hit=stage_proof->Find(stages,index);if(!hit)return false;
                                    const auto local=index-hit->begin;const auto* stage=hit->stage;
                                    if(!stage->uses_direct || !stage->buffers[local].IsDirectEligible())return false;
                                    const auto bytes=stage_proof->Table(hit,prepared->uploads,prepared->upload_bytes,
                                        direct->L1Address(),direct->ZeroPageAddress(),[](const auto& slot){return slot.buffer_type==Shader::BufferType::DirectTable;});
                                    if(bytes.empty())return false;
                                    if(absent_slot(*stage,local,bytes))return true;
                                    if(!Native::Parallel::IsPreparedDirectSlot(bytes,hit->count,local,true,true,info->offset,info->range))return false;
                                    u32 words[4]{};std::memcpy(words,bytes.data()+16+local*16,sizeof(words));
                                    const u64 address=u64(words[0])|(u64(words[1])<<32);
                                    return VideoCore::ParallelDirectViewCoherent(direct,address,words[2]);
                                }
                                const auto count=std::min<std::size_t>(stages.size(),Shader::MaxStageTypes-1);
                                std::size_t base_index=0;
                                for(std::size_t si=0;si<count;++si) {
                                    const auto* stage=stages[si];if(!stage)continue;
                                    const auto n=stage->buffers.size();
                                    if(index<base_index || index-base_index>=n){base_index+=n;continue;}
                                    const auto local=index-base_index;
                                    if(!stage->uses_direct || !stage->buffers[local].IsDirectEligible())return false;
                                    std::size_t table_index=n;
                                    for(std::size_t j=0;j<n;++j)if(stage->buffers[j].buffer_type==Shader::BufferType::DirectTable){if(table_index!=n)return false;table_index=j;}
                                    if(table_index==n || n>(SIZE_MAX-16)/16)return false;
                                    const auto expected=16+n*16;const auto global_table=base_index+table_index;
                                    const PreparedDraw::Upload* table_upload=nullptr;
                                    for(const auto& upload:prepared->uploads)if(upload.info==global_table){if(table_upload)return false;table_upload=&upload;}
                                    if(!table_upload || table_upload->flat_stage>=0 || table_upload->size!=expected ||
                                       table_upload->offset>prepared->upload_bytes.size() || expected>prepared->upload_bytes.size()-table_upload->offset)return false;
                                    const auto* data=prepared->upload_bytes.data()+table_upload->offset;
                                    u32 header[4]{};std::memcpy(header,data,sizeof(header));
                                    const u64 l1=u64(header[0])|(u64(header[1])<<32),zero=u64(header[2])|(u64(header[3])<<32);
                                    if(l1!=direct->L1Address() || zero!=direct->ZeroPageAddress())return false;
                                    const auto bytes=std::span<const std::byte>{reinterpret_cast<const std::byte*>(data),expected};
                                    if(absent_slot(*stage,local,bytes))return true;
                                    if(!Native::Parallel::IsPreparedDirectSlot(bytes,n,local,true,true,info->offset,info->range))return false;
                                    u32 words[4]{};std::memcpy(words,data+16+local*16,sizeof(words));
                                    const u64 address=u64(words[0])|(u64(words[1])<<32);
                                    return VideoCore::ParallelDirectViewCoherent(direct,address,words[2]);
                                }
                                return false;
                            };
                            const bool recorded=TimedProofInvoke(parallel->proof_phases[4],phase_sample,[&] {
                                if(action && action->upload_match!=2) {
                                    ++action_upload_lookups;
                                    return action->upload_match==1 && action->upload_size==info->range;
                                }
                                return std::ranges::any_of(prepared->uploads,[&](const auto& upload){return upload.info==index && upload.size==info->range;});
                            });
                            const auto retained_scan=[&] {return TimedProofInvoke(parallel->proof_phases[5],phase_sample,[&] {return std::ranges::any_of(s.chunks,[&](const auto& chunk){
                                if(!chunk.buffer || !chunk.buffer->Mapped() ||
                                   vk::Buffer{Handle<VkBuffer_T>(chunk.buffer->NativeHandle())}!=info->buffer ||
                                   info->range==VK_WHOLE_SIZE || !info->range)return false;
                                const u64 size=chunk.buffer->Size();return info->offset<=size && info->range<=size-info->offset;
                            });});};
                            const bool retained=recorded && retained_scan();
                            const auto readonly_slot=[&]() -> bool {
                                if(!ParallelReadonlyDescriptorEnabled() || write.descriptorType!=vk::DescriptorType::eStorageBuffer || !info->buffer || !info->range ||
                                   info->range==VK_WHOLE_SIZE || prepared->buffer_ranges.size()>64 || prepared->regions.size()>64)return false;
                                ++parallel->descriptor_readonly_attempts;
                                const auto stages=pipeline->GetStages();const auto stage_count=std::min<std::size_t>(stages.size(),Shader::MaxStageTypes-1);
                                const Shader::Info* matched=planned_stage();std::size_t local=matched?action->stage_local:0,start=0;
                                if(!matched)for(std::size_t si=0;si<stage_count;++si){const auto* stage=stages[si];if(!stage)continue;const auto n=stage->buffers.size();
                                    if(index>=start && index-start<n){matched=stage;local=index-start;break;}start+=n;}
                                if(!matched)return false;const auto& desc=matched->buffers[local];
                                if(desc.IsSpecial() || desc.is_written)return false;
                                const auto sharp=desc.GetSharp(*matched);const u64 base=sharp.base_address,sharp_size=sharp.GetSize();
                                // ClampRangeSize returns the original size below 1 GiB without a map lookup.
                                // Larger ranges remain on the serial path; do not add a recorder map lock.
                                constexpr u64 Limit=u64{1}<<40,NoClampLimit=u64{1}<<30;
                                if(!base || !sharp_size || sharp_size>=NoClampLimit || base>=Limit || sharp_size>Limit-base)return false;
                                const u64 bytes=sharp_size;
                                const auto contains=[&](const auto& range) {
                                    const auto [begin,end]=range;
                                    return begin<=base && end>=base && bytes<=end-base;
                                };
                                bool range_hit=false;
                                const bool contained=Native::Parallel::ReadonlyHintOrScan(prepared->buffer_ranges,
                                    action ? action->range_hint : SIZE_MAX,contains,range_hit);
                                if(range_hit)++parallel->readonly_range_hint_hits;else ++parallel->readonly_range_hint_misses;
                                if(!contained){++parallel->descriptor_readonly_uncontained;return false;}
                                const u64 alignment=r.instance.StorageMinAlignment();if(!alignment)return false;
                                const auto normalized=[&](u64 source_offset,u64 count) {
                                    const u64 aligned=Common::AlignDown(source_offset,alignment),adjust=source_offset-aligned;
                                    return count<=UINT64_MAX-adjust && info->offset==aligned && info->range==count+adjust;
                                };
                                const auto matches_region=[&](const auto& use) {
                                    if(use.address!=base || use.size!=bytes || !use.region || !use.region->buffer || base<use.region->begin)return false;
                                    const auto& buffer=*use.region->buffer;const u64 source_offset=base-use.region->begin;
                                    return vk::Buffer{Handle<VkBuffer_T>(buffer.NativeHandle())}==info->buffer &&
                                       source_offset<=buffer.Size() && bytes<=buffer.Size()-source_offset && normalized(source_offset,bytes);
                                };
                                bool region_hit=false;
                                const bool region_match=Native::Parallel::ReadonlyHintOrScan(prepared->regions,
                                    action ? action->region_hint : SIZE_MAX,matches_region,region_hit);
                                if(region_hit)++parallel->readonly_region_hint_hits;else ++parallel->readonly_region_hint_misses;
                                if(region_match){++parallel->descriptor_readonly_regions;return true;}
                                auto* direct=r.buffer_cache.GetDirectMemory();
                                if(direct){u32 parts=0;bool exact=false;
                                    const bool covered=direct->TryForEachPart(base,bytes,
                                        [&]{return VideoCore::ParallelDirectViewCoherent(direct,base,bytes);},
                                        [&](const VideoCore::Buffer& source,u64 source_offset,u64 part_offset,u64 length){
                                            ++parts;exact=source.Handle()==info->buffer && part_offset==0 && length==bytes && normalized(source_offset,bytes);
                                        });
                                    if(covered && parts==1 && exact){++parallel->descriptor_readonly_imports;return true;}
                                }
                                ++parallel->descriptor_readonly_unmatched;
                                return false;
                            };
                            const auto disabled_clip_null=[&]() -> bool {
                                if(!ParallelReadonlyDescriptorEnabled() || write.descriptorType!=vk::DescriptorType::eStorageBuffer ||
                                   info->buffer || info->offset!=0 || info->range!=VK_WHOLE_SIZE ||
                                   regs.clipper_control.user_clip_plane_enable!=0)return false;
                                if(const auto* stage=planned_stage()) {
                                    const auto& desc=stage->buffers[action->stage_local];
                                    if(desc.buffer_type!=Shader::BufferType::ClipPlanes || desc.is_written)return false;
                                    ++parallel->descriptor_clip_nulls;return true;
                                }
                                const auto stages=pipeline->GetStages();std::size_t start=0;
                                for(std::size_t si=0;si<std::min<std::size_t>(stages.size(),Shader::MaxStageTypes-1);++si){
                                    const auto* stage=stages[si];if(!stage)continue;const auto n=stage->buffers.size();
                                    if(index>=start && index-start<n){const auto& desc=stage->buffers[index-start];
                                        if(desc.buffer_type!=Shader::BufferType::ClipPlanes || desc.is_written)return false;
                                        ++parallel->descriptor_clip_nulls;return true;
                                    }start+=n;
                                }
                                return false;
                            };
                            if(!recorded || !retained){++parallel->direct_slot_attempts;const bool direct_ok=TimedProofInvoke(parallel->proof_phases[6],phase_sample,direct_slot);if(!direct_ok && !TimedProofInvoke(parallel->proof_phases[7],phase_sample,readonly_slot) && !TimedProofInvoke(parallel->proof_phases[8],phase_sample,disabled_clip_null)){
                                descriptor_fail_reason=3;
                                if(ParallelGateDiagEnabled() && parallel->descriptor_samples<4) {
                                    ++parallel->descriptor_samples;u32 stage_id=~u32{0},kind=~u32{0},mode=~u32{0};bool stage_direct=false,eligible=false;
                                    std::size_t start=0;const auto stages=pipeline->GetStages();
                                    for(std::size_t si=0;si<std::min<std::size_t>(stages.size(),Shader::MaxStageTypes-1);++si) {
                                        const auto* stage=stages[si];if(!stage)continue;const auto n=stage->buffers.size();
                                        if(index>=start && index-start<n) {
                                            const auto local=index-start;stage_id=static_cast<u32>(si);const auto& desc=stage->buffers[local];
                                            kind=static_cast<u32>(desc.buffer_type);stage_direct=stage->uses_direct;eligible=desc.IsDirectEligible();
                                            for(std::size_t k=0;k<n;++k)if(stage->buffers[k].buffer_type==Shader::BufferType::DirectTable)
                                                for(const auto& upload:prepared->uploads)if(upload.info==start+k && upload.flat_stage<0 &&
                                                   upload.offset<=prepared->upload_bytes.size() && upload.size<=prepared->upload_bytes.size()-upload.offset &&
                                                   n<=(SIZE_MAX-16)/16 && upload.size==16+n*16) {
                                                    std::memcpy(&mode,prepared->upload_bytes.data()+upload.offset+16+local*16+12,4);
                                                }
                                            break;
                                        }
                                        start+=n;
                                    }
                                    LOG_INFO(Render_Vulkan,"PARALLEL-DESCRIPTOR-SAMPLE index={} type={} null={} offset={} range={} upload={} retained={} stage={} resource_kind={} stage_direct={} direct_eligible={} direct_mode={}",
                                        index,u32(write.descriptorType),!info->buffer,info->offset,info->range,recorded,
                                        recorded ? retained : retained_scan(),stage_id,kind,stage_direct,eligible,mode);
                                }
                                descriptor_safe=false;break;}else if(direct_ok)++parallel->direct_slot_accepted;}
                        }
                        if(!descriptor_safe)break;
                        continue;
                    }
                    if(!known_structure && (write.pBufferInfo || !DescriptorSnapshot::ImageSliceValid(write.pImageInfo,write.descriptorCount,image_infos) ||
                       (write.descriptorType!=vk::DescriptorType::eSampledImage && write.descriptorType!=vk::DescriptorType::eSampler))) {descriptor_fail_reason=4;descriptor_safe=false;break;}
                    for(u32 j=0;j<write.descriptorCount;++j) {
                        const auto& info=write.pImageInfo[j];
                        if(write.descriptorType==vk::DescriptorType::eSampler) {
                            SampledProofTimer phase{parallel->proof_phases[1],phase_sample};
                            if(!info.sampler || info.imageView) {descriptor_fail_reason=5;descriptor_safe=false;break;}
                            bool known=false;
                            static const bool sampler_cache_enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_SAMPLER_PROOF_CACHE");return p && p[0]=='1' && !p[1];}();
                            if(sampler_cache_enabled) {
                                const auto handle_key=reinterpret_cast<u64>(static_cast<VkSampler>(info.sampler));
                                const bool diag=ParallelGateDiagEnabled();
                                known=parallel->sampler_proof.Contains(info.sampler,handle_key);
                                if(known) {if(diag)++parallel->sampler_proof_hits;}
                                else {
                                    if(diag)++parallel->sampler_proof_misses;
                                    std::scoped_lock sampler_lock{sampler_mutex};
                                    for(const auto& [key,value]:samplers) {if(diag)++parallel->sampler_scan_steps;if(value && value->Handle()==info.sampler) {known=true;break;}}
                                    if(known){parallel->sampler_proof.Insert(info.sampler,handle_key);if(diag)++parallel->sampler_scan_positives;}
                                }
                            } else {std::scoped_lock sampler_lock{sampler_mutex};
                                for(const auto& [key,value]:samplers) if(value && value->Handle()==info.sampler) {known=true;break;}}
                            if(!known){descriptor_fail_reason=6;descriptor_safe=false;break;}
                        } else {
                            SampledProofTimer phase{parallel->proof_phases[2],phase_sample};
                            if(!info.imageView || info.sampler || info.imageLayout!=vk::ImageLayout::eGeneral) {
                                if(ParallelGateDiagEnabled()) {
                                    if(!info.imageView)++parallel->sampled_null_view;
                                    if(info.sampler)++parallel->sampled_nonnull_sampler;
                                    if(info.imageLayout!=vk::ImageLayout::eGeneral)++parallel->sampled_nongeneral;
                                    if(!info.imageView && !info.sampler && info.imageLayout==vk::ImageLayout::eGeneral)++parallel->sampled_canonical_null;
                                }
                                static const bool absent_sampled_enabled=[] {const char* p=std::getenv("RYGT_PARALLEL_ABSENT_SAMPLED_IMAGE");return p && p[0]=='1' && !p[1];}();
                                bool absent_sampled=false;
                                const bool canonical_attempt=absent_sampled_enabled && !info.imageView && !info.sampler && info.imageLayout==vk::ImageLayout::eGeneral;
                                if(canonical_attempt && ParallelGateDiagEnabled())++parallel->absent_sampled_attempts;
                                if(absent_sampled_enabled && !info.imageView && !info.sampler &&
                                   info.imageLayout==vk::ImageLayout::eGeneral && r.instance.GetDevice() &&
                                   b.write_sets.size()==b.writes.size()) {
                                    const auto index=static_cast<std::size_t>(&write-b.writes.data());
                                    const auto stages=pipeline->GetStages();
                                    absent_sampled=Native::Parallel::AbsentSampledImage<Shader::Info>(
                                        {stages.data(),stages.size()},b.write_sets[index],write.dstBinding,
                                        write.dstArrayElement,write.descriptorCount,
                                        [](const Shader::Info& stage) {return Shader::Backend::StageSet(u32(stage.sw_stage));},
                                        [](const Shader::Info& stage) -> u64 {
                                            const auto start=Shader::Backend::StageStart(u32(stage.sw_stage));
                                            if(stage.buffers.size()>u64(UINT32_MAX)-start.unified)return UINT64_MAX;
                                            // Only the texture base is needed; avoid planning unused later bindings.
                                            return u64(start.unified)+stage.buffers.size();
                                        });
                                }
                                if(absent_sampled) {if(ParallelGateDiagEnabled())++parallel->absent_sampled_accepted;continue;}
                                if(canonical_attempt && ParallelGateDiagEnabled())++parallel->absent_sampled_rejected;
                                descriptor_fail_reason=7;descriptor_safe=false;break;
                            }
                            const auto full_scan=[&](vk::ImageView wanted) {
                                std::scoped_lock views_lock{views_mutex};
                                for(const auto* entry:b.sampled) {const auto it=views.find(entry);if(sample_view)note_view_cardinality(it==views.end()?0:it->second.size());if(it==views.end())continue;
                                    if(std::ranges::any_of(it->second,[&](const auto& view){return (view.use==ViewUse::Sampled || view.use==ViewUse::CubeSampled) && *view.view==wanted;}))return true;}
                                return false;
                            };
                            bool known=false;
                            if(PreparedSampledProofRequested() && from_prepared && prepared && prepared->pipeline==pipeline &&
                               prepared->proof_context && prepared->proof_context->Valid(prepared->proof_mode_state)) {
                                ++parallel->prepared_view_attempts;
                                const auto slot=static_cast<std::size_t>(&info-b.images.data());
                                known=Native::Parallel::MatchSampledViewProof(prepared->sampled_view_proofs,slot,info.imageView,
                                    sampler_cache_epoch.load(std::memory_order_acquire),b.sampled);
                                if(known)++parallel->prepared_view_accepted;else ++parallel->prepared_view_fallbacks;
                            }
                            if(!known && view_proof_enabled) {
                                const auto hash=[](vk::ImageView h) noexcept {u64 key=reinterpret_cast<u64>(static_cast<VkImageView>(h));key^=key>>33;key*=0xff51afd7ed558ccdULL;key^=key>>33;return key;};
                                const auto walk=[&](std::size_t& cursor,vk::ImageView wanted,auto&& add) {
                                    std::scoped_lock views_lock{views_mutex};
                                    if(parallel->view_applied_mode.Enabled()) return Native::Parallel::BatchViewWalk(cursor,b.sampled.size(),wanted,
                                        [&](std::size_t i,auto&& visit) {
                                            const auto it=views.find(b.sampled[i]);
                                            if(sample_view)note_view_cardinality(it==views.end()?0:it->second.size());
                                            if(it!=views.end())for(const auto& view:it->second) {
                                                const bool sampled=view.use==ViewUse::Sampled || view.use==ViewUse::CubeSampled;
                                                if(!visit(sampled,sampled?*view.view:vk::ImageView{}))return false;
                                            }
                                            return true;
                                        },add);
                                    while(cursor<b.sampled.size()) {
                                        const auto it=views.find(b.sampled[cursor]);if(sample_view)note_view_cardinality(it==views.end()?0:it->second.size());bool found=false;
                                        if(it!=views.end())for(const auto& view:it->second) {
                                            if(view.use!=ViewUse::Sampled && view.use!=ViewUse::CubeSampled)continue;
                                            const auto handle=*view.view;found|=handle==wanted;
                                            if(!add(handle))return false;
                                        }
                                        ++cursor;if(found)return true;
                                    }
                                    return false;
                                };
                                const auto build_wanted=[&](auto&& add) {
                                    // b.images is owned and stable through this draw; do not inspect
                                    // future write pointers before their normal validation.
                                    if(b.images.size()>512)return false;
                                    for(const auto& image:b.images)if(image.imageView && !image.sampler && image.imageLayout==vk::ImageLayout::eGeneral)
                                        if(!add(image.imageView))return false;
                                    return true;
                                };
                                known=view_proof.Query(info.imageView,hash,full_scan,walk,build_wanted);
                            } else if(!known) known=full_scan(info.imageView);
                            if(!known){descriptor_fail_reason=8;descriptor_safe=false;break;}
                        }
                    }
                    if(!descriptor_safe)break;
                }
                parallel->producer_upload_lookup_hits+=action_upload_lookups;
                if(sample_view) {
                    auto& v=parallel->view_sample;++v.draws;if(local_view_stats.queries)++v.queried_draws;
                    v.image_infos+=b.images.size();v.sampled_entries+=b.sampled.size();
                    const auto add=[](auto& to,const auto& from) {to.queries+=from.queries;to.first_hits+=from.first_hits;to.index_hits+=from.index_hits;to.walks+=from.walks;to.fallbacks+=from.fallbacks;to.overflows+=from.overflows;to.wanted_builds+=from.wanted_builds;to.wanted_overflows+=from.wanted_overflows;to.filtered_views+=from.filtered_views;};
                    add(v.proof,local_view_stats);if(gate_view)add(parallel->view_proof_stats,local_view_stats);
                }
                if(stage_proof && ParallelGateDiagEnabled()){parallel->proof_table_builds+=stage_proof->Builds();parallel->proof_table_hits+=stage_proof->Hits();}
                if(descriptor_safe) {
                    if(!b.groups_ready)b.PrepareWriteGroups();
                    const u32 push=pipeline->PushSet();
                    if(push<b.grouped_writes.size() && !b.grouped_writes[push].empty()) {
                        SampledProofTimer phase{parallel->proof_phases[3],phase_sample};
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
                                parallel_push_writes=std::move(owned);++parallel->producer_snapshot_hits;
                                parallel_descriptor_bytes=parallel_push_writes->OwnedBytes()+128;reused_snapshot=true;
                            }
                        }
                        if(!reused_snapshot) {
                            auto snap=DescriptorSnapshot::Make(b.grouped_writes[push],
                                std::span<const vk::DescriptorBufferInfo>{b.buffers.data(),b.buffers.size()},
                                std::span<const vk::DescriptorImageInfo>{b.images.data(),b.images.size()},MaxSnapshotBytes);
                            if(!snap){descriptor_fail_reason=9;descriptor_safe=false;}
                            else {parallel_descriptor_bytes=snap->OwnedBytes()+128;parallel_push_writes=std::make_shared<const DescriptorSnapshot>(std::move(*snap));}
                        }
                    }
                }
            } catch(const std::bad_alloc&) {descriptor_fail_reason=9;descriptor_safe=false;parallel_push_writes.reset();}
        }
        if(sample_preflight) {
            ++parallel->preflight_samples;
            parallel->preflight_wall_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-preflight_begin).count();
            if(preflight_cpu_started && DescriptorThreadCpu100ns(preflight_cpu_end) && preflight_cpu_end>=preflight_cpu_begin) {
                ++parallel->preflight_cpu_samples;parallel->preflight_cpu_100ns+=preflight_cpu_end-preflight_cpu_begin;
            }
        }
        if(cheap_eligible && !descriptor_safe){++parallel->descriptor_rejected;if(ParallelGateDiagEnabled())++parallel->descriptor_fail_reasons[descriptor_fail_reason];}
        if(ParallelGateDiagEnabled()) {
            u64 mask=0;const auto fail=[&](u32 bit,bool bad){if(bad)mask|=u64{1}<<bit;};
            fail(0,!RytyGtsInRace());fail(1,EmulatorSettings.IsPatchShaders());
            fail(2,r.FrameDumpActive() || SceneTrace::Id() || Native::GpuCheckpoints::Requested());
            fail(3,!native_attachment);fail(4,sampled_alias || !feedback.empty());
            fail(5,!extended && targets.state.num_color_attachments!=1);fail(6,prohibited_clear);
            fail(7,!extended && (db.has_depth || db.has_stencil));fail(8,quad_list || quad_triangles || (!extended && is_indexed));
            fail(9,scale_composition || native_fsr_composition);fail(10,vertex_preflight && !vertex_safe && !vi->buffers.empty());
            fail(11,vertex_preflight && !vertex_safe && (!vi->input_bindings.empty() || !vi->attributes.empty()));fail(12,!index_safe);
            fail(13,!prepared || !from_prepared || prepared->pipeline!=pipeline || !prepared->pipeline_keepalive);
            for(const auto* stage:pipeline->GetStages()) if(stage) {
                fail(14,(stage->sw_stage!=Shader::SwStage::Vertex && stage->sw_stage!=Shader::SwStage::Fragment) || stage->uses_patches);
                fail(15,!stage->attribute_flags_known);fail(16,!stage->resource_usage_known);fail(17,stage->translation_failed);
                fail(18,stage->has_fetch_shader && !(vertex_requested && has_vertex_input && (!vertex_preflight || vertex_safe) && stage->sw_stage==Shader::SwStage::Vertex));fail(19,stage->uses_dma || (stage->uses_direct && !ParallelD2Enabled()));
                fail(20,!stage->fmasks.empty() || stage->has_storage_images);fail(21,stage->uses_shader_clock);
                fail(22,std::ranges::any_of(stage->buffers,[](const auto& buffer){return buffer.is_written;}));
            }
            const bool stable=(mask==0)==cheap_eligible;
            fail(23,cheap_eligible && (b.writes_buffers || !b.gpu_writes.empty() || !b.stored.empty()));
            fail(24,cheap_eligible && !descriptor_safe && !b.writes_buffers && b.gpu_writes.empty() && b.stored.empty());
            parallel->gates.Add(mask,stable);
        }
        const bool eligible=descriptor_safe;
        if(eligible) {++parallel->eligible;
            if(parallel->active)++parallel->eligible_in_active_scope;
            else if(s.rendering)++parallel->eligible_in_serial_scope;
            else ++parallel->eligible_at_boundary;
        }
        if(eligible && same_serial_scope)
            parallel->NoteSerialEligible();
        else parallel->FinishSerialEligibleRun();
        if(parallel->active && (!eligible || !s.rendering || !(s.render==targets.state) || parallel->active->draws.size()>=256)) {
            EndRendering();PassBarrier(); // continuation uses LOAD, with an attachment dependency
        }
        const bool split_now=split_candidate && s.rendering && !parallel->active && s.render==targets.state;
        if(eligible && !parallel->active && (!s.rendering || split_now)) {
            // Allocate every packet container before changing logical rendering.
            try {
                if(split_now)++parallel->serial_split_attempts;
                parallel->sync_pins.Collect(r.scheduler.GetWorkSemaphore()->KnownGpuTick(),
                    [&](auto& callback) noexcept {return Native::Rhi::TryEnqueueCompleted(*device,callback);});
                const auto slot=parallel->sync_pins.Reserve();
                if(slot!=Native::Parallel::SyncOwnerPins<>::Invalid) {
                    parallel->pin_slot=slot;
                    struct ReservedPinGuard {
                        ParallelRuntime* parallel;
                        bool committed=false;
                        ~ReservedPinGuard() noexcept {
                            if(!committed) {
                                if(!parallel->sync_pins.CancelEmpty(parallel->pin_slot))std::terminate();
                                parallel->pin_slot=Native::Parallel::SyncOwnerPins<>::Invalid;
                            }
                        }
                    } reserved_pin{parallel};
                    parallel->scratch_draws.reserve(256);
                    const u64 epoch=r.scheduler.CurrentTick();
                    auto lease=std::make_shared<const u64>(epoch);
                    Native::Parallel::Pass pass{};pass.target=targets.state;pass.resource_lease=lease;pass.extended_verified=extended;
                    bool prebind_ok=true;
                    if(split_now) {
                        const auto* prebind_list=s.list;
                        const auto prebind_cmd=s.cmd;
                        parallel_bindings.write_structure_proven=true;
                        set_count_proof();
                        {ScopedParallelBindingCapture capture{parallel_bindings};PushBindings(*pipeline,vk::PipelineBindPoint::eGraphics,b);}
                        if(!b.writes.empty())for(u32 i=0;i<b.grouped_writes.size();++i) {
                            if(i==pipeline->PushSet() || b.grouped_writes[i].empty())continue;
                            parallel_required_sets|=u32{1}<<i;
                            if(!parallel_bindings.sets[i])prebind_ok=false;
                        }
                        prebind_ok=prebind_ok && s.rendering && !parallel->active && s.render==targets.state &&
                            s.list==prebind_list && s.cmd==prebind_cmd && r.scheduler.CurrentTick()==epoch;
                        if(!prebind_ok)parallel->descriptor_updates.Flush(r.instance.GetDevice(),PendingDescriptorUpdates::Abandoned);
                    }
                    if(prebind_ok) {
                        static_assert(std::is_nothrow_move_constructible_v<Native::Parallel::Pass>);
                        pass.draws.swap(parallel->scratch_draws);
                        if(split_now) {EndRendering();PassBarrier();serial_split_started=true;++parallel->serial_split_success;serial_split_prebound=true;}
                        parallel->epoch=epoch;parallel->lease=std::move(lease);parallel->active.emplace(std::move(pass));
                        reserved_pin.committed=true;
                        s.rendering=true;s.render=targets.state;++stats.passes;++parallel->passes;
                    }
                } else ++parallel->owner_slot_pressure;
            } catch(const std::bad_alloc&) {
                if(parallel->pin_slot!=Native::Parallel::SyncOwnerPins<>::Invalid)parallel->sync_pins.CancelEmpty(parallel->pin_slot);
                parallel->pin_slot=Native::Parallel::SyncOwnerPins<>::Invalid;parallel->active.reset();parallel->lease.reset();
            }
        }
        parallel_draw=parallel->active && s.rendering && s.render==targets.state && eligible;
        if(parallel_draw){parallel_index_safe=index_safe;parallel_index_bytes=checked_index_bytes;}
        }
        if(whole_sample) {
            const u64 elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-whole_begin).count();
            ++parallel->whole_samples;parallel->whole_wall_ns+=elapsed;parallel->whole_max_ns=std::max(parallel->whole_max_ns,elapsed);
            if(whole_cpu_started && DescriptorThreadCpu100ns(whole_cpu_end) && whole_cpu_end>=whole_cpu_begin){
                ++parallel->whole_cpu_samples;parallel->whole_cpu_100ns+=whole_cpu_end-whole_cpu_begin;
            }
        }
    }
    // Wait for an existing non-rendering boundary: never restart a clear pass for diagnostics.
    if(preclosed_serial_scope && !parallel_draw)++parallel->natural_serial_fallback;
    if(SceneTrace::Id() && !SceneTrace::state.attempted && !s.rendering && !preclosed_serial_scope) {
        const auto families=r.instance.GetPhysicalDevice().getQueueFamilyProperties();
        const u32 family=r.instance.GetGraphicsQueueFamilyIndex();
        SceneTrace::Prepare(r.instance.GetDevice(),s.cmd,
            family<families.size()?families[family].timestampValidBits:0,
            r.instance.GetPhysicalDevice().getProperties().limits.timestampPeriod);
    }
    admission_partition.Finish();
    {
        PhaseClock part{stats.record_ns[1]};
        if(!parallel_draw) BeginRendering(targets.state);
    }

    // The renderer's dynamic state code, filling this list's state.
    {
        PhaseClock part{stats.record_ns[2]};
        DynamicState* const saved = Rasterizer::t_dynamic_state;
        Rasterizer::t_dynamic_state = &s.dynamic;
        r.ctx->attachment_feedback_loop = feedback.empty() ? vk::ImageAspectFlags{}
                                                           : vk::ImageAspectFlagBits::eColor;
        r.UpdateViewportScissorState();
        r.UpdateDepthStencilState();
        r.UpdatePrimitiveState(is_indexed, quad_triangles);
        r.UpdateRasterizationState();
        r.UpdateColorBlendingState(pipeline);
        if (scale_composition) {
            auto viewports = s.dynamic.viewports;
            auto scissors = s.dynamic.scissors;
            scale_composition = viewports.size() == 1 && scissors.size() == 1 &&
                viewports[0].x == 0.f && viewports[0].width == 1920.f &&
                ((viewports[0].height == 1080.f && viewports[0].y == 0.f) ||
                 (viewports[0].height == -1080.f && viewports[0].y == 1080.f)) &&
                scissors[0].offset.x == 0 && scissors[0].offset.y == 0 &&
                scissors[0].extent.width == 1920 && scissors[0].extent.height == 1080;
            if (scale_composition) {
                viewports[0].x *= 0.5f; viewports[0].y *= 0.5f;
                viewports[0].width *= 0.5f; viewports[0].height *= 0.5f;
                scissors[0].extent = vk::Extent2D{960, 540};
                s.dynamic.SetViewports(viewports);
                s.dynamic.SetScissors(scissors);
            }
        }
        if (native_fsr_composition) {
            auto viewports = s.dynamic.viewports;
            auto scissors = s.dynamic.scissors;
            const float sx = float(fsr_composition->size.width) / targets.state.width;
            const float sy = float(fsr_composition->size.height) / targets.state.height;
            for (auto& v : viewports) {
                v.x *= sx; v.width *= sx; v.y *= sy; v.height *= sy;
            }
            for (auto& sc : scissors) {
                sc.offset = {s32(std::floor(sc.offset.x * sx)), s32(std::floor(sc.offset.y * sy))};
                sc.extent = {u32(std::ceil(sc.extent.width * sx)),
                             u32(std::ceil(sc.extent.height * sy))};
            }
            s.dynamic.SetViewports(viewports);
            s.dynamic.SetScissors(scissors);
        }
        if(!parallel_draw) s.dynamic.Commit(r.instance, CmdBuf{s.cmd});
        Rasterizer::t_dynamic_state = saved;
    }
    {
        PhaseClock part{stats.record_ns[3]};
        if(!parallel_draw) RecordVertexIndex(*vi);
    }
    const u32 index_count = vi->index_count;
    const CmdBuf cmdbuf{s.cmd};
    {
        PhaseClock part{stats.record_ns[4]};
        if(!parallel_draw) s.pipelines.Graphics(pipeline->Handle(), [&] {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
        });
    }
    {
        PhaseClock part{stats.record_ns[5]};
        if(parallel_draw) {
            std::shared_ptr<AheadLease> ahead;std::function<void()> ahead_release;
            std::size_t ahead_slot=parallel->ahead_flights.size();
            if(!serial_split_prebound && from_prepared && prepared && AheadRequested()) {
                ahead=std::static_pointer_cast<AheadLease>(prepared->ahead_descriptor_lease.load(std::memory_order_acquire));
                if(ahead && ahead->context && ahead->lease && ahead->context->mode.load(std::memory_order_acquire)) {
                    for(std::size_t n=0;n<parallel->ahead_flights.size();++n) {
                        const auto i=(parallel->ahead_cursor+n)%parallel->ahead_flights.size();
                        if(!parallel->ahead_flights[i].release){ahead_slot=i;parallel->ahead_cursor=(i+1)%parallel->ahead_flights.size();break;}
                    }
                    if(ahead_slot<parallel->ahead_flights.size()) {
                        try {ahead_release=[owner=ahead]{};parallel_bindings.ahead=ahead.get();}
                        catch(...) {ahead_slot=parallel->ahead_flights.size();}
                    }
                    else ahead->context->capacity.fetch_add(1,std::memory_order_relaxed);
                }
            }
            // Retain before PushBindings: an exception after caching an ahead set must
            // not allow its pool to reset while that cache entry is still reachable.
            if(ahead && ahead_slot<parallel->ahead_flights.size()) {
                parallel->ahead_flights[ahead_slot].lease=ahead;parallel->ahead_flights[ahead_slot].release=std::move(ahead_release);parallel->ahead_flights[ahead_slot].tick=0;++parallel->ahead_live;
            }
            if(!serial_split_prebound) {
                parallel_bindings.write_structure_proven=true; // descriptor_safe validated every source row
                set_count_proof();
                {ScopedParallelBindingCapture capture{parallel_bindings};PushBindings(*pipeline,vk::PipelineBindPoint::eGraphics,b);}
            }
            if(parallel_bindings.ahead_used) {
                ASSERT_MSG(ahead && ahead_slot<parallel->ahead_flights.size(),"ahead descriptor lease missing");
                ahead->used=true;ahead->context->used.fetch_add(1,std::memory_order_relaxed);
            } else if(ahead && ahead_slot<parallel->ahead_flights.size()) {
                // No ahead set reached the cache or command stream.
                auto& flight=parallel->ahead_flights[ahead_slot];flight.lease.reset();flight.release={};--parallel->ahead_live;
            }
            static_assert(Shader::Backend::MaxDescriptorSets<=32);
            bool missing=false;
            if(!b.writes.empty()) for(u32 i=0;i<b.grouped_writes.size();++i) {
                if(i==pipeline->PushSet() || b.grouped_writes[i].empty())continue;
                parallel_required_sets|=u32{1}<<i;
                if(!parallel_bindings.sets[i])missing=true;
            }
            if(missing) {
                // A fresh secondary cannot inherit a serial command buffer's old
                // descriptor bindings. Never enqueue an incomplete packet.
                if(parallel->active->draws.empty()) {
                    parallel->descriptor_updates.Flush(r.instance.GetDevice(),PendingDescriptorUpdates::Abandoned);
                    ASSERT_MSG(parallel->sync_pins.CancelEmpty(parallel->pin_slot),"parallel empty owner slot not empty");
                    parallel->pin_slot=Native::Parallel::SyncOwnerPins<>::Invalid;
                    parallel->scratch_draws.swap(parallel->active->draws);
                    parallel->active.reset();parallel->lease.reset();s.rendering=false;
                    s.dynamic.Invalidate();s.pipelines.Invalidate();s.vertex_state_known=false;
                } else {EndRendering();PassBarrier();}
                if(preclosed_serial_scope)++parallel->natural_binding_skip;
                ++parallel->missing_sets;++stats.skipped;
                return;
            }
        } else PushBindings(*pipeline, vk::PipelineBindPoint::eGraphics, b);
    }
    {
        PhaseClock part{stats.record_ns[6]};
        const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
        const auto [vertex_offset, instance_offset] =
            DrawOffsets(regs, vs_info, pipeline->GetFetchShader());
        const u32 instances = regs.num_instances.NumInstances();
        const auto* fs = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        scene_trace.Programs(vs_info.pgm_hash,fs?fs->pgm_hash:0);
        if(SceneTrace::Id()) {
            const auto& d=s.dynamic;
            r.FrameDumpEvent(fmt::format("scene-occlusion-registers id={} db_render={:08x} db_count={:08x} db_depth_view={:08x} db_override={:08x}",SceneTrace::Id(),regs.reg_array[0xA000],regs.reg_array[0xA001],regs.reg_array[0xA002],regs.reg_array[0xA003]));
            r.FrameDumpEvent(fmt::format("scene-draw id={} vs={:#x} ps={:#x} indices={} instances={} vertex_offset={} instance_offset={} depth_test={} depth_write={} compare={} bias_enable={} depth_bias={}/{}/{} cull={} front={} discard={} viewport_count={} depth_target={:#x} size={}x{} layers={}",
                SceneTrace::Id(),vs_info.pgm_hash,fs?fs->pgm_hash:0,regs.num_indices,instances,vertex_offset,instance_offset,
                d.depth_test_enabled,d.depth_write_enabled,u32(d.depth_compare_op),d.depth_bias_enabled,d.depth_bias_constant,d.depth_bias_slope,d.depth_bias_clamp,
                u32(d.cull_mode),u32(d.front_face),d.rasterizer_discard_enable,d.viewports.size(),targets.depth?targets.depth->info.guest_address:0,targets.state.width,targets.state.height,targets.state.num_layers));
            if(targets.depth)r.FrameDumpEvent(fmt::format("scene-depth id={} address={:#x} format={} clear_depth={} clear_stencil={} write={}",SceneTrace::Id(),targets.depth->info.guest_address,u32(targets.depth->info.pixel_format),targets.state.depth_stencil_attachment.depth_clear,targets.state.depth_stencil_attachment.stencil_clear,d.depth_write_enabled));
            for(size_t cb=0;cb<targets.colors.size();++cb)if(const auto* e=targets.colors[cb])r.FrameDumpEvent(fmt::format("scene-target id={} attachment={} address={:#x} size={}x{} format={} samples={} levels={} layers={} mask={} clear={}",SceneTrace::Id(),cb,e->info.guest_address,e->info.size.width,e->info.size.height,u32(e->info.pixel_format),e->info.num_samples,e->info.resources.levels,e->info.resources.layers,regs.color_target_mask.GetMask(cb),targets.state.color_attachments[cb].is_clear));
            for(size_t vi=0;vi<d.viewports.size();++vi){const auto& v=d.viewports[vi];r.FrameDumpEvent(fmt::format("scene-viewport id={} slot={} xy={}/{} size={}/{} depth={}/{}",SceneTrace::Id(),vi,v.x,v.y,v.width,v.height,v.minDepth,v.maxDepth));}
        }
        if(parallel_draw) {
            auto& draws=parallel->active->draws;
            draws.emplace_back();
            struct UnfinishedDrawGuard {
                std::vector<Native::Parallel::Draw>& draws;
                bool committed=false;
                ~UnfinishedDrawGuard() {if(!committed)draws.pop_back();}
            } unfinished_draw_guard{draws};
            auto& captured=draws.back();
            captured.pipeline=pipeline->Handle();captured.layout=pipeline->GetLayout();
            captured.pipeline_owner=prepared->pipeline_keepalive;captured.dynamic=s.dynamic;captured.push=b.push;
            captured.sets=parallel_bindings.sets;captured.required_sets_mask=parallel_required_sets;
            captured.immutable_upload_buffers_verified=true;captured.push_writes=std::move(parallel_push_writes);
            captured.push_set=pipeline->PushSet()<captured.sets.size()?pipeline->PushSet():static_cast<u32>(captured.sets.size());
            captured.descriptor_bytes=parallel_descriptor_bytes;
            captured.vertex_attribute_count=static_cast<u32>(vi->attributes.size());
            captured.vertex_binding_count=static_cast<u32>(vi->input_bindings.size());
            captured.vertex_buffer_count=static_cast<u32>(vi->buffers.size());
            captured.vertex_backing_verified=true;
            captured.vertex_attributes.assign(vi->attributes.begin(),vi->attributes.end());
            captured.vertex_bindings.assign(vi->input_bindings.begin(),vi->input_bindings.end());
            captured.vertex_buffers.assign(vi->buffers.begin(),vi->buffers.end());
            captured.vertex_offsets.assign(vi->offsets.begin(),vi->offsets.end());
            captured.vertex_sizes.assign(vi->sizes.begin(),vi->sizes.end());
            captured.vertex_strides.assign(vi->strides.begin(),vi->strides.end());
            captured.vertices=regs.num_indices;captured.instances=instances;
            captured.first_vertex=vertex_offset;captured.first_instance=instance_offset;
            if(is_indexed) {captured.indexed=true;captured.index_backing_verified=parallel_index_safe;
                captured.index_buffer=vi->index_buffer;captured.index_offset=vi->index_offset;captured.index_size=parallel_index_bytes;
                captured.index_type=vi->index_type;captured.vertex_offset=s32(vertex_offset);}
            ASSERT_MSG(parallel->sync_pins.Pin(parallel->pin_slot,captured.pipeline_owner),"parallel pipeline owner pin failed");
            if(ParallelGateDiagEnabled()) {const auto n=captured.vertex_buffer_count;
                ++parallel->captured_vertex_counts[n==0?0:n==1?1:n<4?2:n<8?3:n<16?4:n<32?5:6];parallel->captured_vertex_sum+=n;}
            unfinished_draw_guard.committed=true;pending_draw_guard.committed=true;++parallel->draws;
            if(serial_split_started)++parallel->serial_split_first_draws;
            if(preclosed_serial_scope)++parallel->natural_captured;
        } else {
        SceneTrace::GpuBegin(s.cmd);
        Native::GpuCheckpoints::Record(static_cast<VkCommandBuffer>(s.cmd), 1, frame,
            vs_info.pgm_hash, fs ? fs->pgm_hash : 0, regs.num_indices);
        if (quad_triangles) {
            if (index_count != 0) {
                cmdbuf.drawIndexed(index_count, instances, 0, s32(vertex_offset),
                                   instance_offset);
            }
        } else if (is_indexed) {
            cmdbuf.drawIndexed(regs.num_indices, instances, 0, s32(vertex_offset),
                               instance_offset);
        } else {
            cmdbuf.draw(regs.num_indices, instances, vertex_offset, instance_offset);
        }
        SceneTrace::GpuEnd(s.cmd);
        }
    }
    {
        PhaseClock part{stats.record_ns[7]};
        NoteWrites(&targets, b);
    }
    RecordPartitionClock tail_partition{parallel?&parallel->record_partition[partition_mode][1]:nullptr};
    if (!feedback.empty()) {
        EndRendering();
        feedback_layout(vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT,
                        vk::ImageLayout::eGeneral);
        s.buffers_written = true;
    }

    // DIAG-208 (RYGT_PIXEL_TRACE=1): while a frame is dumped, the scene target's traced pixels
    // are copied after each draw into it, to name the draw that first writes a wrong colour.
    static const bool pixel_trace = [] {
        const char* env = std::getenv("RYGT_PIXEL_TRACE");
        return env && env[0] == '1';
    }();
    if (pixel_trace && r.FrameDumpCollecting() && targets.colors[0] &&
        targets.colors[0]->info.guest_address == TracedTarget &&
        g_pixel_trace.hashes.size() < PixelTraceDraws) {
        if (!g_pixel_trace.buffer) {
            g_pixel_trace.buffer = device->CreateBuffer(
                {.size = PixelTraceDraws * TracedPixels.size() * 4,
                 .usage = Native::Rhi::BufferUsageStorage | Native::Rhi::BufferUsageTransferDst,
                 .memory = Native::Rhi::MemoryKind::Readback,
                 .name = "pixel trace"});
        }
        if (g_pixel_trace.buffer && g_pixel_trace.buffer->Mapped()) {
            EndRendering();
            GlobalBarrier();
            const u64 base = g_pixel_trace.hashes.size() * TracedPixels.size() * 4;
            std::array<vk::BufferImageCopy, TracedPixels.size()> copies;
            for (size_t i = 0; i < copies.size(); ++i) {
                copies[i] = vk::BufferImageCopy{
                    .bufferOffset = base + i * 4,
                    .imageSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor,
                                      .mipLevel = 0,
                                      .baseArrayLayer = 0,
                                      .layerCount = 1},
                    .imageOffset = {TracedPixels[i].first, TracedPixels[i].second, 0},
                    .imageExtent = {1, 1, 1}};
            }
            stream.cmd.copyImageToBuffer(
                vk::Image{Handle<VkImage_T>(targets.colors[0]->image->NativeHandle())},
                vk::ImageLayout::eGeneral,
                vk::Buffer{Handle<VkBuffer_T>(g_pixel_trace.buffer->NativeHandle())}, copies);
            GlobalBarrier();
            const auto* vs = pipeline->GetStages()[u32(Shader::SwStage::Vertex)];
            const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
            g_pixel_trace.hashes.emplace_back(vs ? vs->pgm_hash : 0, ps ? ps->pgm_hash : 0);
        }
    }

    if (native_fsr_composition) FinishNativeFsr(targets);
    else CaptureNativeFsr(*pipeline, targets, b);

    if (scale_composition) {
        EndRendering();
        GlobalBarrier();
        const vk::Image scratch{Handle<VkImage_T>(composition_scratch->NativeHandle())};
        const vk::Image output{Handle<VkImage_T>(targets.colors[0]->image->NativeHandle())};
        if (!composition_scratch_initialized) {
            const vk::ImageMemoryBarrier2 transition{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = scratch,
                .subresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
            s.cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                    .pImageMemoryBarriers = &transition});
            composition_scratch_initialized = true;
        }
        {
            const vk::ImageCopy copy{
                .srcSubresource{vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .dstSubresource{vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .extent{960, 540, 1}};
            s.cmd.copyImage(output, vk::ImageLayout::eGeneral, scratch,
                            vk::ImageLayout::eGeneral, copy);
            s.list->Barrier(Native::Rhi::ScopeTransfer, Native::Rhi::ScopeTransfer);
            vk::ImageBlit blit{};
            blit.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            blit.dstSubresource = blit.srcSubresource;
            blit.srcOffsets[1] = vk::Offset3D{960, 540, 1};
            blit.dstOffsets[1] = vk::Offset3D{1920, 1080, 1};
            s.cmd.blitImage(scratch, vk::ImageLayout::eGeneral, output,
                            vk::ImageLayout::eGeneral, blit, vk::Filter::eLinear);
        }
        s.list->Barrier(Native::Rhi::ScopeAll, Native::Rhi::ScopeAll);
        s.written.push_back(targets.colors[0]);
        static std::atomic<u32> scale_logged{};
        if (Sample(scale_logged)) {
            LOG_WARNING(Render_Vulkan, "COMPOSITION-SCALE: 960x540 shading, 1920x1080 output; HUD unchanged");
        }
    }

    ++stats.draws;
    ++s.commands;
    tail_partition.Finish();
    {RecordPartitionClock chunk_partition{parallel?&parallel->record_partition[partition_mode][2]:nullptr};ChunkFlushCheck();}
    {RecordPartitionClock report_partition{parallel?&parallel->record_partition[partition_mode][3]:nullptr};Report();}
}

bool NativeRenderer::PrepareEnabled() {
    // PERF-316: on with the native renderer (+12% fps in a Time Attack on a Ryzen 5 5500 and
    // RX 7600: the recorder records draws game job threads worked out). RYGT_NATIVE_PREPARE=0 or
    // =1 overrides it.
    static const bool enabled = [] {
        if (const char* env = std::getenv("RYGT_NATIVE_PREPARE"); env && *env) {
            return env[0] == '1' && NativeModeRequested();
        }
        return NativeModeRequested();
    }();
    return enabled;
}

void NativeRenderer::PreparedDraw::Reset() {
    packet = nullptr;
    epoch = 0;
    pipeline = nullptr;
    if(AheadRequested())ahead_descriptor_lease.store({},std::memory_order_release);
    cpu_proof = {};
    descriptor_actions.clear(); // retain capacity for the next prepared draw
    sampled_view_proofs.clear();
    proof_context.reset();proof_mode_state=0;
    pipeline_keepalive.reset();
    stage_hashes = {};
    bindings.Clear();
    regions.clear();
    image_ranges.clear();
    buffer_ranges.clear();
    upload_bytes.clear();
    uploads.clear();
    null_images = 0;
    has_vertices = false;
    vertex_hash = 0;
    indexed = false;
    index_offset = 0;
    index_address = 0;
    mirrors.clear();
    in_place.clear();
    for (auto& copy : stage_copies) {
        copy.present = false;
    }
}

void NativeRenderer::ChangeLog::Note(VAddr begin, VAddr end) {
    const u64 next = seq.load(std::memory_order_relaxed) + 1;
    entries[next % entries.size()] = {next, begin, end};
    seq.store(next, std::memory_order_release);
}

bool NativeRenderer::ChangeLog::Untouched(u64 since,
                                          std::span<const std::pair<VAddr, VAddr>> ranges) const {
    const u64 now = seq.load(std::memory_order_acquire);
    if (now == since) {
        return true;
    }
    if (now - since > entries.size()) {
        return false;
    }
    const FreshnessRangeSearch<VAddr> search{ranges};
    for (u64 at = since + 1; at <= now; ++at) {
        const Entry& entry = entries[at % entries.size()];
        if (search.Intersects(entry.begin, entry.end)) return false;
    }
    return true;
}

NativeRenderer::PreparedDraw* NativeRenderer::NewPrepared() {
    std::scoped_lock lock{prepared_pool_mutex};
    if (!prepared_free.empty()) {
        PreparedDraw* draw = prepared_free.back();
        prepared_free.pop_back();
        return draw;
    }
    prepared_all.push_back(std::make_unique<PreparedDraw>());
    return prepared_all.back().get();
}

void NativeRenderer::FreePrepared(PreparedDraw* draw) {
    draw->Reset();
    std::scoped_lock lock{prepared_pool_mutex};
    prepared_free.push_back(draw);
}

bool NativeRenderer::HasPrepared(const void* packet) {
    if (!RytyGtsInRace()) return false; // required menu work stays on the recorder
    if (!packet) {
        return false;
    }
    PreparedShard& shard = ShardFor(packet);
    std::scoped_lock lock{shard.mutex};
    return shard.draws.contains(packet);
}

const GraphicsPipeline* NativeRenderer::InstallPrepared(PreparedDraw& p) {
    // As AcquireGraphicsPipeline with the command thread's selection: the stages' user data
    // and flattened user data, the flattened buffers moved in (and the old ones kept here).
    std::array<Rasterizer::SelectedStage, Shader::MaxStageTypes> selected{};
    for (u32 i = 0; i < selected.size(); ++i) {
        const auto& copy = p.stage_copies[i];
        auto& stage = selected[i];
        stage.present = copy.present;
        if (!copy.present) {
            continue;
        }
        stage.pgm_base = copy.pgm_base;
        stage.num_user_data = copy.num_user_data;
        std::copy_n(copy.user_data.begin(), copy.num_user_data, stage.user_data.begin());
        stage.flattened = std::move(p.stage_copies[i].flattened);
    }
    const bool installed = r.InstallDrawStages(p.pipeline, selected, true);
    for (u32 i = 0; i < selected.size(); ++i) {
        p.stage_copies[i].flattened = std::move(selected[i].flattened);
    }
    if (!installed) {
        return nullptr;
    }
    ++stats.prepared_installed;
    return p.pipeline;
}

NativeRenderer::PreparedDraw* NativeRenderer::TakePrepared(const void* packet) {
    if (!packet) {
        return nullptr;
    }
    PreparedShard& shard = ShardFor(packet);
    std::scoped_lock lock{shard.mutex};
    const auto it = shard.draws.find(packet);
    if (it == shard.draws.end()) {
        return nullptr;
    }
    PreparedDraw* draw = it->second;
    shard.draws.erase(it);
    return draw;
}

u64 NativeRenderer::StageHash(const Shader::Info& info) {
    // What the bindings were worked out from: the stage's sharps (the user data registers the
    // program doesn't read hold whatever earlier work left, which a job and the command
    // processor need not agree on).
    static thread_local std::vector<u32> words;
    words.clear();
    const auto add = [](const auto& sharp) {
        const auto* data = reinterpret_cast<const u32*>(&sharp);
        words.insert(words.end(), data, data + sizeof(sharp) / sizeof(u32));
    };
    for (const auto& desc : info.buffers) {
        if (!desc.IsSpecial()) {
            add(desc.GetSharp(info));
        }
    }
    for (const auto& desc : info.images) {
        add(desc.GetSharp(info));
    }
    for (const auto& desc : info.samplers) {
        add(desc.GetSharp(info));
    }
    return Native::Parallel::AdaptiveXXH3Seed(words.data(), words.size() * sizeof(u32), info.pgm_base);
}

void NativeRenderer::PrepareJobDraw(const void* packet, u32 epoch, const AmdGpu::Regs& regs,
                                    const GraphicsPipeline& pipeline,
                                    std::shared_ptr<const void> pipeline_keepalive,
                                    std::span<const Shader::Info* const> stages, bool is_indexed,
                                    u32 index_offset) {
    if (!RytyGtsInRace()) return; // do not retain speculative menu bindings
    AheadPrepareGuard ahead_guard{FindAhead(this)};
    auto proof_guard=PreparedSampledProofRequested()?prepared_proof_contexts.TryEnter(this):Native::Parallel::ProofProducerGuard{};

    const auto start = std::chrono::steady_clock::now();
    PreparedDraw* p = NewPrepared();
    p->proof_context=proof_guard.context;p->proof_mode_state=proof_guard.state;
    p->packet = packet;
    p->epoch = epoch;
    p->pipeline = &pipeline;
    p->pipeline_keepalive = std::move(pipeline_keepalive);
    // What the lookups depend on, read before them.
    p->image_generation = registry->Generation();
    p->buffer_seq = buffer_log.Seq();
    p->unmap_gen = unmap_generation.load(std::memory_order_acquire);
    p->bindings.push = UserData(regs);
    const u32 count = std::min<u32>(static_cast<u32>(stages.size()), Shader::MaxStageTypes - 1);
    std::array<StageResourceBindings, Shader::MaxStageTypes> plans{};
    bool ok = true;
    size_t total = 0;
    for (u32 i = 0; i < count; ++i) {
        if (const auto* stage = stages[i]) {
            total += stage->buffers.size() + stage->images.size() + stage->samplers.size();
        }
    }
    p->bindings.writes.reserve(total);
    for (u32 i = 0; i < count; ++i) {
        // OPT-012: what the recorder installs (as the command thread's selection would copy).
        auto& copy = p->stage_copies[i];
        const auto* stage = stages[i];
        copy.present = stage != nullptr;
        if (stage) {
            copy.pgm_base = stage->pgm_base;
            copy.num_user_data =
                static_cast<u32>(std::min<size_t>(stage->user_data.size(), copy.user_data.size()));
            std::copy_n(stage->user_data.begin(), copy.num_user_data, copy.user_data.begin());
            copy.flattened.assign(stage->flattened_ud_buf.begin(), stage->flattened_ud_buf.end());
        }
    }
    for (u32 i = 0; ok && i < count; ++i) {
        const auto* stage = stages[i];
        p->stage_hashes[i] = stage ? StageHash(*stage) : 0;
        if (!stage) {
            continue;
        }
        plans[i] = PlanStageResourceBindings(*stage,
                                             Shader::Backend::StageStart(u32(stage->sw_stage)));
        auto buffer_binding = plans[i].buffers;
        ok = PrepareBuffers(*stage, regs, buffer_binding, *p);
    }
    for (u32 i = 0; ok && i < count; ++i) {
        if (const auto* stage = stages[i]) {
            auto texture_binding = plans[i].textures;
            ok = PrepareTextures(*stage, regs, texture_binding, *p);
        }
    }
    if (!ok) {
        ++prepare_stats.failed;
        FreePrepared(p);
        return;
    }
    // OPT-011: the vertex and index buffers too, when they need nothing ordered.
    constexpr u32 VertexStage = static_cast<u32>(Shader::SwStage::Vertex);
    if (VertexStage < count && stages[VertexStage]) {
        p->has_vertices =
            PrepareVertices(*p, regs, pipeline, *stages[VertexStage], is_indexed, index_offset);
    }
    // Finish lookup work on the preparing thread. The recorder checks the same
    // memory union and refreshes each exact Region pointer only once.
    if (DrawPreparationEnabled(1)) {
        CompactPreparedRanges(p->image_ranges);
        CompactPreparedRanges(p->buffer_ranges);
    }
    if (DrawPreparationEnabled(2)) StableUniquePrepared(p->regions);
    p->bindings.PrepareWriteGroups(); // binding-owned spans, published with this draw
    p->bindings.PrepareDescriptorKeys(pipeline);
    if (ParallelRecordingRequested()) try {
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
    if(auto context=ahead_guard.context;context && p->pipeline_keepalive && context->mode.load(std::memory_order_acquire)) {
        context->attempts.fetch_add(1,std::memory_order_relaxed);
        try {
            auto candidate=std::make_shared<AheadLease>();candidate->context=context;candidate->pipeline_owner=p->pipeline_keepalive;candidate->pipeline_layout=pipeline.GetLayout();
            std::array<vk::DescriptorSetLayout,Shader::Backend::MaxDescriptorSets> layouts{};
            std::array<u32,Shader::Backend::MaxDescriptorSets> indices{};std::array<std::size_t,4> counts{};std::size_t n=0;bool supported=true;
            const auto& groups=p->bindings.grouped_writes;
            // Validate both the selected job schema and the pipeline's immutable
            // canonical schema against every ordinary write. DynamicIndex counts
            // depend on mutable T# values and cannot be safely speculated here.
            const auto schema_matches=[&](auto stage_list) {
                std::array<bool,Shader::Backend::MaxDescriptorSets> seen_sets{};
                bool valid=groups.size()<=layouts.size();
                for(const auto* stage:stage_list)if(valid && stage) {
                    const auto set=Shader::Backend::StageSet(u32(stage->sw_stage));
                    if(set>=groups.size() || seen_sets[set]){valid=false;break;}seen_sets[set]=true;
                    for(const auto& image:stage->images)if(image.mip_fallback_mode==Shader::MipStorageFallbackMode::DynamicIndex){valid=false;break;}
                    if(!valid || set==pipeline.PushSet())continue;
                    const auto& group=groups[set];std::size_t at=0;u32 binding=0;
                    const auto expect=[&](vk::DescriptorType type,u32 n){
                        if(at>=group.size())return false;const auto& w=group[at++];
                        const bool ok=!w.pNext && w.dstBinding==binding && w.dstArrayElement==0 && w.descriptorType==type && w.descriptorCount==n;
                        if(n>UINT32_MAX-binding)return false;binding+=n;return ok;
                    };
                    for(const auto& buffer:stage->buffers){(void)buffer;if(!expect(vk::DescriptorType::eStorageBuffer,1)){valid=false;break;}}
                    for(const auto& image:stage->images)if(!expect(image.is_written?vk::DescriptorType::eStorageImage:vk::DescriptorType::eSampledImage,1)){valid=false;break;}
                    for(const auto& sampler:stage->samplers){(void)sampler;if(!expect(vk::DescriptorType::eSampler,1)){valid=false;break;}}
                    if(at!=group.size())valid=false;
                }
                for(std::size_t i=0;valid && i<groups.size();++i)if(i!=pipeline.PushSet() && !groups[i].empty() && !seen_sets[i])valid=false;
                return valid;
            };
            const auto& canonical=pipeline.CanonicalStages();
            constexpr std::size_t graphics_stages=Shader::MaxStageTypes-1;
            if(stages.size()<graphics_stages || canonical.size()<graphics_stages)supported=false;
            if(supported) supported=schema_matches(std::span<const Shader::Info* const>{stages.data(),graphics_stages}) &&
                schema_matches(std::span<const Shader::Info* const>{canonical.data(),graphics_stages});
            for(u32 i=0;supported && i<groups.size();++i) {
                if(groups[i].empty() || i==pipeline.PushSet())continue;
                auto layout=pipeline.SetLayout(i);if(!layout || n==layouts.size()){supported=false;break;}
                layouts[n]=layout;indices[n++]=i;candidate->layouts[i]=layout;
                for(const auto& w:groups[i]) {
                    std::size_t type=0;
                    switch(w.descriptorType){case vk::DescriptorType::eStorageBuffer:type=0;break;case vk::DescriptorType::eSampledImage:type=1;break;case vk::DescriptorType::eStorageImage:type=2;break;case vk::DescriptorType::eSampler:type=3;break;default:supported=false;continue;}
                    if(w.descriptorCount>AheadVulkanBackend::MaxDescriptorsPerPool[type]-counts[type]){supported=false;break;}
                    counts[type]+=w.descriptorCount;
                }
            }
            if(supported && n) {
                AheadPools::Failure reason{};
                auto lease=context->pools.TryPrepare(std::span<const vk::DescriptorSetLayout>{layouts.data(),n},counts,p->pipeline_keepalive,r.scheduler.GetWorkSemaphore()->KnownGpuTick(),&reason);
                if(lease) {for(std::size_t k=0;k<n;++k)candidate->sets[indices[k]]=lease->sets[k];candidate->prepared_sets=n;candidate->lease=std::move(lease);p->ahead_descriptor_lease.store(std::move(candidate),std::memory_order_release);context->ready.fetch_add(1,std::memory_order_relaxed);}
                else if(reason==AheadPools::Failure::Capacity)context->capacity.fetch_add(1,std::memory_order_relaxed);
                else if(reason!=AheadPools::Failure::Disabled)context->failures.fetch_add(1,std::memory_order_relaxed);
            } else context->capacity.fetch_add(1,std::memory_order_relaxed);
        } catch(...) {context->failures.fetch_add(1,std::memory_order_relaxed);}
    }
    // An off/on transition invalidates all tokens from the old mode generation.
    // A transition racing this check is also rejected by the recorder's Valid check.
    if(p->proof_context && !p->proof_context->Valid(p->proof_mode_state)) {
        p->sampled_view_proofs.clear();p->proof_context.reset();p->proof_mode_state=0;
    }
    std::vector<PreparedDraw*> dropped;
    {
        PreparedShard& shard = ShardFor(packet);
        std::scoped_lock lock{shard.mutex};
        if (shard.draws.size() > 1024) {
            // Draws the recorder never took (not submitted, or filtered): from a few
            // submissions ago, they go.
            for (auto it = shard.draws.begin(); it != shard.draws.end();) {
                if (epoch - it->second->epoch > 4) {
                    dropped.push_back(it->second);
                    it = shard.draws.erase(it);
                } else {
                    ++it;
                }
            }
        }
        const auto [it, inserted] = shard.draws.try_emplace(packet, p);
        if (!inserted) {
            dropped.push_back(it->second);
            it->second = p;
            ++prepare_stats.replaced;
        }
    }
    prepare_stats.stale += dropped.size();
    for (PreparedDraw* draw : dropped) {
        FreePrepared(draw);
    }
    ++prepare_stats.made;
    prepare_stats.ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
}

std::optional<std::pair<vk::Buffer, u64>> NativeRenderer::JobGuestBuffer(VAddr address, u64 size,
                                                                         bool write,
                                                                         PreparedDraw& p) {
    {
        std::shared_lock map_lock{buffer_map_mutex};
        if (Region* region = RegionOver(address, size)) {
            if (address < region->begin || address + size > region->end) {
                return std::nullopt; // the region grows over it first
            }
            p.regions.push_back({region, address, size});
            return std::pair{vk::Buffer{Handle<VkBuffer_T>(region->buffer->NativeHandle())},
                             u64{address - region->begin}};
        }
    }
    if (write) {
        return std::nullopt; // a region is made for it (NAT-144), or mirrors dropped
    }
    auto* direct = r.buffer_cache.GetDirectMemory();
    if (direct && direct->EnsureImported(address, size)) {
        if (const auto found = direct->FindBuffer(address, size)) {
            return std::pair{found->first->Handle(), found->second};
        }
    }
    return std::nullopt; // gathered or copied, in GPU order
}

bool NativeRenderer::PrepareBuffers(const Shader::Info& stage, const AmdGpu::Regs& regs,
                                    Shader::Backend::Bindings& binding, PreparedDraw& p) {
    // As BindBuffers, without its side effects.
    // DIAG-205 (RYGT_RECORDER_SHADER=<hash>): that program's draws go to the recorder, which
    // reads their buffers later (tests whether the job threads read per-frame constants early).
    static const u64 recorder_hash = [] {
        const char* env = std::getenv("RYGT_RECORDER_SHADER");
        return env && *env ? std::strtoull(env, nullptr, 16) : 0;
    }();
    if (recorder_hash != 0 && stage.pgm_hash == recorder_hash) {
        return false; // the recorder
    }
    DrawBindings& b = p.bindings;
    const u64 alignment = r.instance.StorageMinAlignment();
    // Match BindBuffers: prepared and recorder-side draws must choose the same path.
    static const bool composition_descriptors = [&] {
        const char* env = std::getenv("RYGT_COMPOSITION_BUFFER_DESCRIPTORS");
        return env && *env ? env[0] == '1' : r.instance.GetVendorID() == 0x1002;
    }();
    const bool descriptor_composition = composition_descriptors &&
        stage.sw_stage == Shader::SwStage::Fragment && stage.pgm_hash == 0xb33ec4df;
    static const bool postprocess_descriptors = [&] {
        const char* env = std::getenv("RYGT_POSTPROCESS_BUFFER_DESCRIPTORS");
        return env && *env ? env[0] == '1' : r.instance.GetVendorID() == 0x1002;
    }();
    const bool descriptor_postprocess = postprocess_descriptors &&
        stage.sw_stage == Shader::SwStage::Fragment &&
        (stage.pgm_hash == 0x184b619c || stage.pgm_hash == 0x7cd92fb8);
    // Diagnostic A/B: retain shader compilation but use descriptor bindings.
    static const bool descriptor_only_a82d2fe = [] {
        const char* value = std::getenv("RYGT_A82D2FE_DESCRIPTOR_ONLY");
        return value && value[0] == '1';
    }();
    const bool force_descriptor = descriptor_only_a82d2fe &&
        stage.sw_stage == Shader::SwStage::Compute && stage.pgm_hash == 0xa82d2fe;
    static std::atomic<bool> descriptor_diagnostic_logged{};
    if (force_descriptor && !descriptor_diagnostic_logged.exchange(true))
        LOG_WARNING(Render_Vulkan, "DIAG: 0xa82d2fe descriptor-only buffer binding active");
    auto* direct = stage.uses_direct && !descriptor_composition && !force_descriptor
        ? r.buffer_cache.GetDirectMemory() : nullptr;
    static thread_local std::vector<std::array<u32, 4>> direct_entries;
    direct_entries.assign(stage.buffers.size(), {});
    constexpr u64 GpuAddressLimit = 1ULL << 40;
    const auto upload = [&](const void* data, u32 size) {
        const u32 offset = static_cast<u32>(p.upload_bytes.size());
        const auto* bytes = static_cast<const u8*>(data);
        p.upload_bytes.insert(p.upload_bytes.end(), bytes, bytes + size);
        p.uploads.push_back({static_cast<u32>(b.buffers.size()), offset, size});
        return vk::DescriptorBufferInfo{VK_NULL_HANDLE, 0, size};
    };
    u32 index = 0;
    for (const auto& desc : stage.buffers) {
        const u32 buffer_index = index++;
        vk::DescriptorBufferInfo info{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
        if (desc.IsSpecial()) {
            switch (desc.buffer_type) {
            case Shader::BufferType::GdsBuffer:
                info = {Handle<VkBuffer_T>(gds->NativeHandle()), 0, gds->Size()};
                break;
            case Shader::BufferType::Flatbuf: {
                // The recorder's own copy of the stage's user data (the registers the program
                // doesn't read may differ here), uploaded at commit.
                const u32 size = static_cast<u32>(stage.flattened_ud_buf.size() * sizeof(u32));
                if (size != 0) {
                    p.uploads.push_back({static_cast<u32>(b.buffers.size()), 0, size,
                                         static_cast<s32>(stage.sw_stage)});
                    info = vk::DescriptorBufferInfo{VK_NULL_HANDLE, 0, size};
                }
                break;
            }
            case Shader::BufferType::ClipPlanes:
                if (regs.clipper_control.user_clip_plane_enable != 0) {
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = regs.clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    info = upload(planes.data(), sizeof(planes));
                }
                break;
            case Shader::BufferType::DirectTable: {
                const u32 words = static_cast<u32>(4 + stage.buffers.size() * 4);
                static thread_local std::vector<u32> table;
                table.assign(words, 0);
                if (auto* memory = r.buffer_cache.GetDirectMemory()) {
                    const u64 l1 = memory->L1Address();
                    const u64 zero = memory->ZeroPageAddress();
                    table[0] = u32(l1);
                    table[1] = u32(l1 >> 32);
                    table[2] = u32(zero);
                    table[3] = u32(zero >> 32);
                    for (size_t i = 0; i < stage.buffers.size(); ++i) {
                        std::memcpy(&table[4 + i * 4], direct_entries[i].data(), 16);
                    }
                }
                info = upload(table.data(), words * sizeof(u32));
                break;
            }
            default:
                return false; // compute-only kinds and the unsupported ones: the recorder
            }
        } else {
            const auto vsharp = desc.GetSharp(stage);
            const u64 base = vsharp.base_address;
            const u64 sharp_size = vsharp.GetSize();
            // DIAG-204 (RYGT_PROBE_SHADER=<hash>): that program's buffers and their first bytes.
            static const u64 probe_hash = [] {
                const char* env = std::getenv("RYGT_PROBE_SHADER");
                return env && *env ? std::strtoull(env, nullptr, 16) : 0;
            }();
            if (probe_hash != 0 && stage.pgm_hash == probe_hash) {
                static std::atomic<u32> probes{};
                if (const u32 n = probes++; n < 64) {
                    std::array<u32, 16> head{};
                    const bool read = base != 0 && Core::RytyMemory::GuardedCopy(
                        head.data(), reinterpret_cast<const void*>(base), sizeof(head));
                    LOG_WARNING(Render_Vulkan,
                                "DIAG-204: {:#x} buffer {} V# base {:#x} stride {} records {} "
                                "dfmt {} nfmt {} swizzle {} tid {}: {}",
                                stage.pgm_hash, buffer_index, base, vsharp.GetStride(),
                                vsharp.num_records, u32(vsharp.GetDataFmt()),
                                u32(vsharp.GetNumberFmt()), u32(vsharp.swizzle_enable),
                                u32(vsharp.add_tid_enable),
                                read ? fmt::format("{:08x}", fmt::join(head, " "))
                                     : std::string{"unreadable"});
                }
            }
            const bool impossible =
                vsharp.num_records != UINT32_MAX &&
                (sharp_size >= GpuAddressLimit || base + sharp_size > GpuAddressLimit ||
                 (sharp_size >= 1_GB && base != 0 && !r.memory->IsValidMapping(base)));
            const bool unmapped =
                !impossible && base != 0 && sharp_size != 0 && !r.IsMappedStart(base);
            if (base != 0 && sharp_size != 0 && !impossible && !unmapped) {
                const u64 size = r.memory->ClampRangeSize(base, sharp_size);
                bool region_over;
                {
                    std::shared_lock map_lock{buffer_map_mutex};
                    region_over = RegionOver(base, size) != nullptr;
                }
                p.buffer_ranges.emplace_back(base, base + size);
                if (direct && !(descriptor_postprocess && !desc.is_written) &&
                    desc.IsDirectEligible() && size < (u64{1} << 32) && !region_over &&
                    direct->EnsureImported(base, size)) {
                    direct_entries[buffer_index] = {u32(base), u32(base >> 32), u32(size), 1u};
                } else if (const auto found = JobGuestBuffer(base, size, desc.is_written, p)) {
                    const u64 aligned = Common::AlignDown(found->second, alignment);
                    const u64 adjust = found->second - aligned;
                    b.push.AddOffset(binding.buffer, static_cast<u32>(adjust));
                    info = {found->first, aligned, size + adjust};
                    b.writes_buffers |= desc.is_written;
                    if (desc.is_written) {
                        b.gpu_writes.emplace_back(base, size); // NAT-119
                    }
                } else {
                    return false;
                }
            }
        }
        b.buffers.push_back(info);
        b.write_sets.push_back(u8(Shader::Backend::StageSet(u32(stage.sw_stage)))); // SHD-002
        b.writes.push_back(vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding.unified++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &b.buffers.back(),
        });
        ++binding.buffer;
    }
    return true;
}

bool NativeRenderer::PrepareTextures(const Shader::Info& stage, const AmdGpu::Regs& regs,
                                     Shader::Backend::Bindings& binding, PreparedDraw& p) {
    // As BindTextures, without its side effects: pictures that exist only, no rewrites or
    // copies of sources (the recorder does them).
    DrawBindings& b = p.bindings;
    const auto warm_upload = [&](VAddr address, u64 size) {
        // Only establish aliases/imports here; never refresh, copy, upload or
        // alter image state on the worker. The recorder still validates freshness
        // and orders every actual upload. Keep this bounded to real texture data.
        if (size == 0 || size > 64_MB) {
            return;
        }
        auto* direct = r.buffer_cache.GetDirectMemory();
        if (!direct) {
            return;
        }
        {
            std::shared_lock lock{buffer_map_mutex};
            if (const Region* region = RegionOver(address, size);
                region && address >= region->begin && size <= region->end - address) {
                return; // already has a GPU source; no guest import needed
            }
        }
        Native::SlowCallTrace<1> trace{true, "prefetch-image-import", {"import"}};
        trace.Next(0, address, size);
        direct->WarmImported(address, size);
    };
    const u64 now_frame = shared_frame.load(std::memory_order_relaxed);
    static thread_local std::array<TextureMemo, 4096> memos{};
    using ProofMemoArray=std::array<Native::Parallel::SampledMemoProof<vk::ImageView>,4096>;
    static_assert(std::tuple_size_v<ProofMemoArray> == std::tuple_size_v<decltype(memos)>);
    static thread_local std::unique_ptr<ProofMemoArray> proof_memos;
    const bool proof_enabled=p.proof_context && p.proof_context->Valid(p.proof_mode_state);
    if(proof_enabled && !proof_memos)proof_memos.reset(new(std::nothrow) ProofMemoArray{});
    const u64 proof_epoch=proof_enabled ? sampler_cache_epoch.load(std::memory_order_acquire) : 0;
    for (const auto& image_desc : stage.images) {
        auto tsharp = image_desc.GetSharp(stage);
        if (!MatchExperimentalCompositionTexture(tsharp, stage, image_desc, *registry)) {
            return false; // Wait for preceding target registrations on the recorder.
        }
        const bool cube_sampling = image_desc.post_op == Shader::SharpFetchPostOp::NativeCubeSampling;
        const u32 num_bindings = image_desc.NumBindings(stage);
        const u32 first = static_cast<u32>(b.images.size());
        const bool storage = image_desc.is_written;
        const auto add_write = [&] {
            b.write_sets.push_back(u8(Shader::Backend::StageSet(u32(stage.sw_stage)))); // SHD-002
            b.writes.push_back(vk::WriteDescriptorSet{
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = binding.unified,
                .dstArrayElement = 0,
                .descriptorCount = num_bindings,
                .descriptorType =
                    storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage,
                .pImageInfo = &b.images[first],
            });
            binding.unified += num_bindings;
        };
        const auto bind_null = [&] {
            for (u32 i = 0; i < num_bindings; ++i) {
                b.images.push_back({VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral});
            }
            ++p.null_images;
            add_write();
        };
        std::array<u64, 4> raw_sharp{};
        static_assert(sizeof(tsharp) == sizeof(raw_sharp));
        std::memcpy(raw_sharp.data(), &tsharp, sizeof(raw_sharp));
        const u32 memo_flags = u32(storage) | (u32(image_desc.is_array) << 1) |
                               (u32(image_desc.is_depth) << 2) |
                               (u32(image_desc.mip_fallback_mode) << 3) |
                               ((image_desc.constant_mip_index & 0xff) << 6) |
                               (num_bindings << 16) | (u32(cube_sampling) << 31);
        if (num_bindings > 4 || stage.pgm_hash == 0xaa3822a3) {
            return false; // the recorder (FIX-081's grass shader, long arrays)
        }
        const u64 memo_generation = p.image_generation + p.unmap_gen;
        TextureMemo& memo = memos[XXH3_64bits_withSeed(raw_sharp.data(), sizeof(raw_sharp),
                                                       memo_flags) &
                                  (memos.size() - 1)];
        auto* proof_memo=proof_enabled && proof_memos ? &(*proof_memos)[static_cast<std::size_t>(&memo-memos.data()) & (proof_memos->size()-1)] : nullptr;
        const VAddr address = tsharp.Address();
        if (memo.entry && memo.flags == memo_flags && memo.sharp == raw_sharp &&
            memo.generation == memo_generation) {
            Native::ImageEntry* entry = memo.entry;
            if (!storage && entry->needs_upload.load(std::memory_order_relaxed)) {
                warm_upload(entry->info.guest_address, entry->info.guest_size);
            }
            entry->last_use.store(now_frame, std::memory_order_relaxed);
            for (u32 i = 0; i < num_bindings; ++i) {
                b.images.push_back({VK_NULL_HANDLE, memo.views[i], vk::ImageLayout::eGeneral});
            }
            (storage ? b.stored : b.sampled).push_back(entry);
            if(proof_memo && !storage && p.proof_context->Valid(p.proof_mode_state) && sampler_cache_epoch.load(std::memory_order_acquire)==proof_epoch) {
                bool known=proof_memo->Matches(this,&memo,entry,proof_epoch,p.image_generation,p.unmap_gen,raw_sharp,memo_flags,cube_sampling,memo.views);
                if(!known) {
                    // An ordinary memo can predate proof mode. Rebuild its proof only
                    // from live owner records; contention simply retains fallback.
                    std::unique_lock views_lock{views_mutex,std::try_to_lock};
                    if(views_lock.owns_lock()) {
                        const auto it=views.find(entry);
                        known=it!=views.end() && num_bindings!=0;
                        for(u32 i=0;known && i<num_bindings;++i) {
                            const auto wanted=memo.views[i];
                            known=bool(wanted) && std::ranges::any_of(it->second,[&](const auto& view) {
                                return (view.use==ViewUse::Sampled || view.use==ViewUse::CubeSampled) && view.view && *view.view==wanted;
                            });
                        }
                        if(known && p.proof_context->Valid(p.proof_mode_state) && sampler_cache_epoch.load(std::memory_order_acquire)==proof_epoch)
                            *proof_memo={this,&memo,entry,proof_epoch,p.image_generation,p.unmap_gen,raw_sharp,memo_flags,cube_sampling,memo.views};
                        else known=false;
                    }
                }
                if(known)
                    for(u32 i=0;i<num_bindings;++i)Native::Parallel::AddSampledViewProof(p.sampled_view_proofs,first+i,b.sampled.size()-1,memo.views[i],entry,proof_epoch,cube_sampling);
            }
            p.image_ranges.emplace_back(entry->info.guest_address, entry->info.guest_address + std::max<u64>(entry->info.guest_size, 1));
            add_write();
            continue;
        }
        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (address == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid ||
            !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt) ||
            !r.memory->IsValidGpuMapping(address, 0) ||
            LiverpoolToVK::TrySurfaceFormat(data_fmt, num_fmt) == vk::Format::eUndefined ||
            VideoCore::CheckImageDescriptorGeometry(tsharp, r.instance.GetImageLimits()) !=
                VideoCore::ImageDescriptorGeometryError::None) {
            bind_null();
            continue;
        }
        VideoCore::TextureCache::ImageDesc desc{tsharp, image_desc};
        const bool original_backing=HasImageBacking(desc.info.guest_address,desc.info.guest_size);
        const u64 original_address=desc.info.guest_address;
        const u32 original_first_mip=desc.view_info.range.base.level;
        const bool resident_view=!original_backing && !storage && !cube_sampling && num_bindings==1 &&
            image_desc.mip_fallback_mode==Shader::MipStorageFallbackMode::None &&
            RebaseResidentPrtView(desc,[&](u64 address,u64 bytes){return HasImageBacking(address,bytes);});
        if (resident_view && SceneTrace::Id()) r.FrameDumpEvent(fmt::format(
            "scene-prt-resident-view id={} program={:#x} original={:#x} first_mip={} resolved={:#x} bytes={} size={}x{} levels={}",
            SceneTrace::Id(),stage.pgm_hash,original_address,original_first_mip,desc.info.guest_address,
            desc.info.guest_size,desc.info.size.width,desc.info.size.height,desc.info.resources.levels));
        // A valid base address is not sufficient: stale optional descriptors can
        // span gigabytes beyond their allocation. Reject before image creation,
        // guest-memory import, staging allocation or a detiling dispatch.
        if (!original_backing && !resident_view) {
            static std::atomic<u32> logged{};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
                LOG_WARNING(Render_Vulkan,
                    "IMAGE-BACKING: rejected ps/vs {:#x} image {:#x}+{:#x} {}x{}x{} pitch {} "
                    "mips {} layers {} tile {} bits {}; raw {:016x} {:016x} {:016x} {:016x}",
                    stage.pgm_hash, desc.info.guest_address, desc.info.guest_size,
                    desc.info.size.width, desc.info.size.height, desc.info.size.depth,
                    desc.info.pitch, desc.info.resources.levels, desc.info.resources.layers,
                    u32(desc.info.tile_mode), desc.info.num_bits,
                    raw_sharp[0], raw_sharp[1], raw_sharp[2], raw_sharp[3]);
            }
            bind_null();
            continue;
        }

        if (!r.IsMappedStart(desc.info.guest_address)) {
            bind_null();
            continue;
        }
        Native::ImageEntry* entry = registry->Find(desc.info, storage, now_frame, false);
        if (!storage && (!entry || entry->needs_upload.load(std::memory_order_relaxed))) {
            warm_upload(desc.info.guest_address, desc.info.guest_size);
        }
        if (!entry) {
            return false; // made by the recorder, in GPU order
        }
        if (storage && !(vk::ImageUsageFlags{entry->backend_usage} &
                         vk::ImageUsageFlagBits::eStorage)) {
            return false; // Let the recorder handle the unsupported storage binding.
        }
        const auto fallback = image_desc.mip_fallback_mode;
        for (u32 i = 0; i < num_bindings; ++i) {
            auto view_info = desc.view_info;
            if (fallback == Shader::MipStorageFallbackMode::ConstantIndex) {
                view_info.range.base.level += image_desc.constant_mip_index;
                view_info.range.extent.levels = 1;
            } else if (fallback == Shader::MipStorageFallbackMode::DynamicIndex) {
                view_info.range.base.level += i;
                view_info.range.extent.levels = 1;
            }
            const vk::ImageView view =
                View(*entry, view_info, storage ? ViewUse::Storage
                                               : cube_sampling ? ViewUse::CubeSampled
                                                               : ViewUse::Sampled);
            b.images.push_back({VK_NULL_HANDLE, view, vk::ImageLayout::eGeneral});
        }
        memo.sharp = raw_sharp;
        memo.flags = memo_flags;
        memo.entry = entry;
        for (u32 i = 0; i < num_bindings; ++i) {
            memo.views[i] = b.images[first + i].imageView;
        }
        memo.generation = memo_generation;
        if(proof_memo) {
            if(!storage && p.proof_context->Valid(p.proof_mode_state) && sampler_cache_epoch.load(std::memory_order_acquire)==proof_epoch)
                *proof_memo={this,&memo,entry,proof_epoch,p.image_generation,p.unmap_gen,raw_sharp,memo_flags,cube_sampling,memo.views};
            else *proof_memo={};
        }
        (storage ? b.stored : b.sampled).push_back(entry);
        if(proof_enabled && !storage && p.proof_context->Valid(p.proof_mode_state) && sampler_cache_epoch.load(std::memory_order_acquire)==proof_epoch)
            for(u32 i=0;i<num_bindings;++i)Native::Parallel::AddSampledViewProof(p.sampled_view_proofs,first+i,b.sampled.size()-1,memo.views[i],entry,proof_epoch,cube_sampling);
        p.image_ranges.emplace_back(desc.info.guest_address,
                                    desc.info.guest_address +
                                        std::max<u64>(desc.info.guest_size, 1));
        add_write();
    }
    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (!ssharp.Valid() || (ssharp.border_color_type.Value() == AmdGpu::BorderColor::Custom &&
                                regs.ta_bc_base.Address() == 0)) {
            ssharp = AmdGpu::Sampler{};
        }
        b.images.push_back({SamplerFor(ssharp, regs.ta_bc_base, sampler.is_depth), VK_NULL_HANDLE,
                            vk::ImageLayout::eGeneral});
        b.write_sets.push_back(u8(Shader::Backend::StageSet(u32(stage.sw_stage)))); // SHD-002
        b.writes.push_back(vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding.unified++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampler,
            .pImageInfo = &b.images.back(),
        });
    }
    return true;
}

u64 NativeRenderer::VertexHash(const GraphicsPipeline& pipeline, const Shader::Info& vs,
                                u32 step_rate_0, u32 step_rate_1) {
    // The V#s the fetch shader reads (what the vertex buffers were worked out from).
    const auto& fetch = pipeline.GetFetchShader();
    std::array<u32, 2 + 4 * MaxVertexBufferCount> words{step_rate_0, step_rate_1};
    u32 count = 2;
    if (!fetch.Empty()) {
        for (const auto& attrib : fetch.attributes) {
            if (count + 4 > words.size()) {
                break;
            }
            const auto sharp = attrib.GetSharp(vs);
            static_assert(sizeof(sharp) == 4 * sizeof(u32));
            std::memcpy(&words[count], &sharp, sizeof(sharp));
            count += 4;
        }
    }
    return Native::Parallel::AdaptiveXXH3(words.data(), count * sizeof(u32));
}

std::optional<std::pair<vk::Buffer, u64>> NativeRenderer::JobVertexBuffer(VAddr address, u64 size,
                                                                          PreparedDraw& p,
                                                                          Mirror** mirror_out) {
    // As GuestBufferLookup for vertices and indices (a region, else a mirror, else in place).
    // The caller holds buffer_map_mutex shared.
    *mirror_out = nullptr;
    {
        if (Region* region = RegionOver(address, size)) {
            if (address < region->begin || address + size > region->end) {
                return std::nullopt;
            }
            p.regions.push_back({region, address, size});
            return std::pair{vk::Buffer{Handle<VkBuffer_T>(region->buffer->NativeHandle())},
                             u64{address - region->begin}};
        }
        static const bool mirrors_on = [] {
            const char* env = std::getenv("RYGT_NATIVE_MIRRORS");
            return !(env && env[0] == '0');
        }();
        if (mirrors_on && size <= 64_MB) {
            if (Mirror* mirror = MirrorHolding(address, size)) {
                if (std::ranges::find(p.mirrors, mirror) == p.mirrors.end()) {
                    p.mirrors.push_back(mirror);
                }
                *mirror_out = mirror;
                return std::pair{vk::Buffer{Handle<VkBuffer_T>(mirror->buffer->NativeHandle())},
                                 u64{address - mirror->begin}};
            }
        }
    }
    auto* direct = r.buffer_cache.GetDirectMemory();
    if (direct && direct->EnsureImported(address, size)) {
        if (const auto found = direct->FindBuffer(address, size)) {
            p.in_place.emplace_back(address, size);
            return std::pair{found->first->Handle(), found->second};
        }
    }
    return std::nullopt;
}

bool NativeRenderer::PrepareVertices(PreparedDraw& p, const AmdGpu::Regs& regs,
                                     const GraphicsPipeline& pipeline, const Shader::Info& vs,
                                     bool is_indexed, u32 index_offset) {
    if (regs.primitive_type == AmdGpu::PrimitiveType::QuadList) {
        return false; // the recorder makes their triangles
    }
    if (!p.vertex_index) {
        p.vertex_index = std::make_unique<VertexIndexState>();
    }
    VertexIndexState& v = *p.vertex_index;
    v = {};
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline.GetVertexInputs(v.attributes, v.input_bindings, divisors, guest_buffers,
                             regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1, vs);
    p.vertex_hash = VertexHash(pipeline, vs, regs.vgt_instance_step_rate_0,
                               regs.vgt_instance_step_rate_1);
    // Regions and mirrors stay put while this thread reads them.
    std::shared_lock map_lock{buffer_map_mutex};
    Mirror* near = nullptr;
    for (const auto& guest : guest_buffers) {
        std::optional<std::pair<vk::Buffer, u64>> found;
        u64 size = 0;
        const VAddr base = guest.base_address;
        const u64 sharp_size = guest.GetSize();
        if (near && base != 0 && sharp_size > 0 && base >= near->begin &&
            base + sharp_size <= near->end) {
            size = sharp_size; // OPT-004 on this thread
            found = std::pair{vk::Buffer{Handle<VkBuffer_T>(near->buffer->NativeHandle())},
                              u64{base - near->begin}};
        } else if (base != 0 && sharp_size > 0 && r.memory->MappedAreaEnd(base, base + 1) != 0) {
            size = r.memory->ClampRangeSize(base, sharp_size);
            found = JobVertexBuffer(base, size, p, &near);
            if (!found) {
                return false;
            }
        }
        if (size != 0) {
            p.buffer_ranges.emplace_back(base, base + size);
        }
        v.buffers.push_back(found ? found->first : vk::Buffer{});
        v.offsets.push_back(found ? found->second : 0);
        v.sizes.push_back(found ? size : 0);
        v.strides.push_back(guest.GetStride());
    }
    p.indexed = is_indexed;
    p.index_offset = index_offset;
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    v.index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    v.index_count = regs.num_indices;
    if (is_indexed) {
        const VAddr address =
            regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size;
        const u64 bytes = u64(regs.num_indices) * index_size;
        if (bytes == 0) {
            return false;
        }
        Mirror* unused = nullptr;
        const auto found = JobVertexBuffer(address, bytes, p, &unused);
        if (!found) {
            return false;
        }
        p.buffer_ranges.emplace_back(address, address + bytes);
        p.index_address = address;
        v.index_buffer = found->first;
        v.index_offset = found->second;
    }
    return true;
}

bool NativeRenderer::CommitPreparedVertices(PreparedDraw& p, const GraphicsPipeline& pipeline,
                                            const AmdGpu::Regs& regs, bool is_indexed,
                                            u32 index_offset, bool quad_triangles,
                                            bool trusted) {
    // (The ranges were checked with the bindings'.)
    const auto reject = [&] {
        ++stats.prepared_vertices_rejected;
        return false;
    };
    if (quad_triangles || p.indexed != is_indexed || p.index_offset != index_offset) {
        return reject();
    }
    if (!trusted &&
        VertexHash(pipeline, pipeline.GetStage(Shader::SwStage::Vertex),
                   regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1) != p.vertex_hash) {
        return reject();
    }
    const auto& v = *p.vertex_index;
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    if (v.index_count != regs.num_indices ||
        v.index_type != (is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32)) {
        return reject();
    }
    if (is_indexed) {
        const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
        if (p.index_address !=
            regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size) {
            return reject();
        }
    }
    // The ordered part: pages the CPU wrote into the mirrors, and the ranges read in place
    // (noted as mirror candidates).
    for (Mirror* mirror : p.mirrors) {
        RefreshMirror(*mirror);
        mirror->used_frame = frame;
        ++stats.mirror_binds;
    }
    for (const auto& [address, size] : p.in_place) {
        MirrorBuffer(address, size);
    }
    ++stats.prepared_vertices;
    return true;
}

bool NativeRenderer::CommitPrepared(PreparedDraw& p, const GraphicsPipeline& pipeline,
                                    const AmdGpu::Regs& regs, bool trusted) {
    Native::SlowCallTrace<8> trace{t_timed, "commit-prepared",
        {"stage-validation", "image-freshness", "sort-cpu-writes", "buffer-freshness",
         "refresh-regions", "upload-data", "image-rewrite-sources", "push-data"}};
    const auto reject = [&](u32 why) {
        ++stats.prepared_rejected[why];
        return false;
    };
    if (p.pipeline != &pipeline) {
        return reject(0);
    }
    const auto stages = pipeline.GetStages();
    // OPT-012: stages installed from this prepared draw are what it was worked out from.
    for (u32 i = 0; !trusted && i < stages.size() && i < p.stage_hashes.size(); ++i) {
        if ((stages[i] ? StageHash(*stages[i]) : 0) != p.stage_hashes[i]) {
            return reject(1);
        }
    }
    for (const auto& upload : p.uploads) {
        if (upload.flat_stage >= 0) {
            const Shader::Info* stage = stages[upload.flat_stage];
            if (!stage || stage->flattened_ud_buf.size() * sizeof(u32) != upload.size) {
                return reject(1);
            }
        }
    }
    if (p.unmap_gen != unmap_generation.load(std::memory_order_acquire)) {
        return reject(2);
    }
    trace.Next(1);
    if (!registry->Untouched(p.image_generation, p.image_ranges)) {
        return reject(3);
    }
    trace.Next(2);
    SortRegionWrites(); // CPU writes since (may drop mirrors)
    trace.Next(3);
    if (!buffer_log.Untouched(p.buffer_seq, p.buffer_ranges)) {
        return reject(4);
    }
    // The ordered part, as BindBuffers and BindTextures do it.
    trace.Next(4);
    for (const auto& use : p.regions) {
        RefreshRegion(*use.region, use.address, use.size);
        ++stats.region_binds;
    }
    const u64 alignment = r.instance.StorageMinAlignment();
    trace.Next(5);
    for (const auto& upload : p.uploads) {
        const u8* source = p.upload_bytes.data() + upload.offset;
        u32 size = upload.size;
        if (upload.flat_stage >= 0) {
            source = reinterpret_cast<const u8*>(stages[upload.flat_stage]->flattened_ud_buf.data());
        }
        if (upload.flat_stage >= 0) {
            p.bindings.buffers[upload.info] =
                UploadFlatData(static_cast<u32>(upload.flat_stage), source, size);
        } else {
            const auto [buffer, offset, data] = Allocate(size, alignment);
            std::memcpy(data, source, size);
            p.bindings.buffers[upload.info] = vk::DescriptorBufferInfo{buffer, offset, size};
        }
    }
    trace.Next(6);
    for (const auto* stored : p.bindings.stored) {
        auto& entry = *const_cast<Native::ImageEntry*>(stored);
        entry.last_use.store(frame, std::memory_order_relaxed);
        ApplyRewrite(entry); // NAT-120
    }
    for (const auto* sampled : p.bindings.sampled) {
        auto& entry = *const_cast<Native::ImageEntry*>(sampled);
        entry.last_use.store(frame, std::memory_order_relaxed);
        ApplyRewrite(entry); // NAT-120, NAT-133
        if (entry.from_cpu) {
            RefreshSources(entry); // NAT-104
        }
    }
    // RefreshRegion can process CPU writes again after the first freshness
    // check and retire a prepared mirror. Settle any new stream/promotion,
    // then reject the whole prepared draw before selecting its bindings or
    // dereferencing its vertex mirrors if any prepared range changed.
    Begin();
    if (p.unmap_gen != unmap_generation.load(std::memory_order_acquire)) return reject(2);
    if (!buffer_log.Untouched(p.buffer_seq, p.buffer_ranges)) return reject(4);
    // The viewport transform the shaders get, from the registers as recorded.
    trace.Next(7);
    const Shader::PushData fresh = UserData(regs);
    p.bindings.push.xoffset = fresh.xoffset;
    p.bindings.push.xscale = fresh.xscale;
    p.bindings.push.yoffset = fresh.yoffset;
    p.bindings.push.yscale = fresh.yscale;
    p.bindings.push.quad_sample_mask = fresh.quad_sample_mask;
    stats.null_images += p.null_images;
    ++stats.prepared_used;
    return true;
}

void NativeRenderer::GpuWrote(VAddr address, u64 size, const std::array<u32, 2>* pattern,
                              bool exact) {
    const u64 clock = ++write_clock; // NAT-148: when the memory was rewritten
    registry->GpuWrote(address, size, pattern, exact, clock);
    // A fill of a stencil plane: depth images whose stencil is there clear it at next use.
    const auto it = stencil_owners.find(address);
    if (it == stencil_owners.end() || !pattern) {
        return;
    }
    const u32 lo = (*pattern)[0];
    const bool uniform = (*pattern)[0] == (*pattern)[1] && lo == (lo & 0xffu) * 0x01010101u;
    if (!uniform) {
        return;
    }
    for (auto* entry : it->second) {
        entry->stencil_clear_pending = true;
        entry->stencil_clear_value = static_cast<u8>(lo);
        entry->rewrite_clock = std::max(entry->rewrite_clock, clock);
    }
}

void NativeRenderer::TrimFillPatterns(u64 current_key) {
    // Fill values can change throughout a race. Cache recent patterns within a bounded
    // budget; unlike rendered images, these buffers can always be regenerated.
    constexpr u64 PatternBudget = 64ull << 20;
    u64 total = 0;
    for (const auto& [key, pattern] : fill_patterns) {
        total += pattern.buffer->Size();
    }
    while (total > PatternBudget && fill_patterns.size() > 1) {
        auto oldest = fill_patterns.end();
        for (auto it = fill_patterns.begin(); it != fill_patterns.end(); ++it) {
            if (it->first != current_key &&
                (oldest == fill_patterns.end() || it->second.last_use < oldest->second.last_use)) {
                oldest = it;
            }
        }
        if (oldest == fill_patterns.end()) break;
        total -= oldest->second.buffer->Size();
        // RHI destruction retires the buffer after recorded GPU work completes.
        fill_patterns.erase(oldest);
    }
}

const Native::Rhi::Buffer* NativeRenderer::PatternBuffer(u32 lo, u32 hi, u64 bytes) {
    // NAT-134: a device buffer holding lo, hi, lo, hi... for fills, made once per pattern and
    // grown when a bigger picture needs it (only ever read afterwards).
    const u64 key = u64(lo) | (u64(hi) << 32);
    const auto found = fill_patterns.find(key);
    if (found != fill_patterns.end() && found->second.buffer->Size() >= bytes) {
        found->second.last_use = ++fill_pattern_clock;
        return found->second.buffer.get();
    }
    const u64 size = std::max<u64>(Common::AlignUp(bytes, 64_KB), 1_MB);
    auto buffer = device->CreateBuffer({.size = size,
                                        .usage = Native::Rhi::BufferUsageTransferDst |
                                                 Native::Rhi::BufferUsageTransferSrc,
                                        .memory = Native::Rhi::MemoryKind::DeviceLocal,
                                        .name = "native fill pattern"});
    if (!buffer) {
        return found != fill_patterns.end() ? found->second.buffer.get() : nullptr;
    }
    Stream& s = Begin();
    EndRendering();
    const vk::Buffer handle{Handle<VkBuffer_T>(buffer->NativeHandle())};
    const auto transfer_barrier = [&] {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
        };
        s.cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    };
    if (lo == hi) {
        s.cmd.fillBuffer(handle, 0, VK_WHOLE_SIZE, lo);
    } else {
        // 8 KB of the pattern written in, then doubled by copies until the buffer is full.
        std::array<u32, 2048> words;
        for (size_t i = 0; i < words.size(); i += 2) {
            words[i] = lo;
            words[i + 1] = hi;
        }
        u64 filled = sizeof(words);
        s.cmd.updateBuffer(handle, 0, filled, words.data());
        while (filled < size) {
            transfer_barrier();
            const u64 chunk = std::min(filled, size - filled);
            s.cmd.copyBuffer(handle, handle, vk::BufferCopy{.srcOffset = 0, .dstOffset = filled,
                                                            .size = chunk});
            filled += chunk;
        }
    }
    transfer_barrier();
    ++s.commands;
    auto& slot = fill_patterns[key];
    // Replaced buffers follow the same fence retirement as evicted cache entries.
    slot.buffer = std::move(buffer);
    slot.last_use = ++fill_pattern_clock;
    TrimFillPatterns(key);
    return slot.buffer.get();
}

bool NativeRenderer::FillImage(Native::ImageEntry& entry, u32 lo, u32 hi, u32 rows) {
    // NAT-132/134: a picture filled with the pattern (two dwords, repeated), as memory holds it
    // after a fill: copied in from a buffer holding it (any format: every texel is the pattern's
    // bytes). `rows` != 0: only those rows of the first mip and layer (NAT-124).
    const auto& info = entry.info;
    const u32 texel = info.props.is_depth
                          ? (info.pixel_format == vk::Format::eD16Unorm ||
                                     info.pixel_format == vk::Format::eD16UnormS8Uint
                                 ? 2u
                                 : 4u)
                          : info.num_bits / 8;
    if (texel == 0 || info.props.is_block || info.num_samples != 1 || !entry.image ||
        (lo != hi && texel < 8 && texel != 4)) {
        return false;
    }
    const auto& desc = entry.image->Desc();
    const u32 layers = desc.dim == Native::Rhi::ImageDim::D3 ? 1u : std::max(desc.layers, 1u);
    const u32 depth = desc.dim == Native::Rhi::ImageDim::D3 ? std::max(desc.depth, 1u) : 1u;
    const u32 mips = rows ? 1u : std::max(desc.mips, 1u);
    const u32 height0 = rows ? std::min(rows, desc.height) : desc.height;
    const u64 bytes = u64(desc.width) * height0 * texel * (rows ? 1u : layers * depth);
    const Native::Rhi::Buffer* pattern = PatternBuffer(lo, hi, bytes);
    if (!pattern || pattern->Size() < bytes) {
        return false;
    }
    Stream& s = Begin();
    GlobalBarrier(); // the work before is done with the picture
    const vk::Buffer buffer{Handle<VkBuffer_T>(pattern->NativeHandle())};
    boost::container::small_vector<vk::BufferImageCopy, 16> copies;
    for (u32 m = 0; m < mips; ++m) {
        const u32 w = std::max(desc.width >> m, 1u);
        const u32 h = rows ? height0 : std::max(desc.height >> m, 1u);
        const u32 d = std::max(depth >> m, 1u);
        copies.push_back(vk::BufferImageCopy{
            .bufferOffset = 0,
            .bufferRowLength = w,
            .bufferImageHeight = h,
            .imageSubresource{.aspectMask = info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                                : vk::ImageAspectFlagBits::eColor,
                              .mipLevel = m,
                              .baseArrayLayer = 0,
                              .layerCount = rows ? 1u : layers},
            .imageOffset = {0, 0, 0},
            .imageExtent = {w, h, d},
        });
    }
    s.cmd.copyBufferToImage(buffer, vk::Image{Handle<VkImage_T>(entry.image->NativeHandle())},
                            vk::ImageLayout::eGeneral, static_cast<u32>(copies.size()),
                            copies.data());
    Written(&entry, entry.rewrite_clock); // NAT-148
    s.buffers_written = true; // the next work waits for the fill
    ++s.commands;
    return true;
}

void NativeRenderer::ApplyRewrite(Native::ImageEntry& entry) {
    registry->QueueUploadForUse(entry);
    using Rewrite = Native::ImageEntry::Rewrite;
    if (entry.rewrite == Rewrite::None && !entry.stencil_clear_pending) {
        return;
    }
    const auto& info = entry.info;
    if (Native::WatchedTexture(info.guest_address, info.guest_address + 1) &&
        Native::WatchSample()) {
        LOG_WARNING(Render_Vulkan, "FIX-153 watch: apply {} to {} {:#x} ({:#x} bytes of {:#x}, "
                    "pattern {:#x} {:#x})",
                    entry.rewrite == Rewrite::Clear    ? "clear"
                    : entry.rewrite == Rewrite::Upload ? "upload"
                                                       : "stencil clear",
                    entry.from_cpu ? "texture" : "target", info.guest_address, entry.rewrite_bytes,
                    info.guest_size, entry.rewrite_pattern[0], entry.rewrite_pattern[1]);
    }
    const Rewrite rewrite = std::exchange(entry.rewrite, Rewrite::None);
    const bool stencil = std::exchange(entry.stencil_clear_pending, false) &&
                         info.props.has_stencil && entry.image;
    const u32 lo = entry.rewrite_pattern[0];
    const u32 hi = entry.rewrite_pattern[1];
    const bool partial = rewrite == Rewrite::Clear && entry.rewrite_bytes < info.guest_size;
    // What a clear command can do: a depth plane filled with one value (32-bit float, or 16-bit
    // unorm), a color picture filled with zeros.
    std::optional<float> depth_value;
    bool color_zero = false;
    if (rewrite == Rewrite::Clear && !partial) {
        if (info.props.is_depth) {
            const auto format = info.pixel_format;
            const bool d16 = format == vk::Format::eD16Unorm || format == vk::Format::eD16UnormS8Uint;
            if (!d16 && lo == hi) {
                depth_value = std::bit_cast<float>(lo);
            } else if (d16 && lo == hi && (lo >> 16) == (lo & 0xffffu)) {
                depth_value = float(lo & 0xffffu) / 65535.0f;
            }
        } else if (!info.props.is_block && lo == 0 && hi == 0) {
            color_zero = true;
        }
    }
    if (rewrite == Rewrite::Clear && partial) {
        // NAT-124: a fill of the first bytes only: those rows of the first mip and layer.
        const u32 texel = info.props.is_depth ? 4u : std::max(info.num_bits / 8, 1u);
        const u64 row_bytes = u64(std::max(info.pitch, info.size.width)) * texel;
        const u32 rows = u32(std::min<u64>((entry.rewrite_bytes + row_bytes - 1) / row_bytes,
                                           info.size.height));
        if (rows != 0 && FillImage(entry, lo, hi, rows)) {
            ++stats.rewrite_partial;
        }
    } else if (rewrite == Rewrite::Clear && !depth_value && !color_zero) {
        // NAT-132: other patterns (GT Sport fills pictures in its scratch pool with non-zero
        // values) are filled in on the GPU; uploading them from memory cost 1-3 GB a second in
        // the menus.
        if (FillImage(entry, lo, hi, 0)) {
            ++stats.rewrite_fills;
        } else if (info.num_samples == 1) {
            registry->RequestUpload(entry); // in GPU order, before the next draw or dispatch
            ++stats.rewrite_uploads;
        }
    } else if (rewrite == Rewrite::Upload && info.num_samples == 1) {
        registry->RequestUpload(entry);
        ++stats.rewrite_uploads;
    }
    if (!depth_value && !color_zero && !stencil) {
        return;
    }
    Stream& s = Begin();
    GlobalBarrier(); // the work before is done with the picture
    const vk::Image image{Handle<VkImage_T>(entry.image->NativeHandle())};
    const auto range = [](vk::ImageAspectFlags aspect) {
        return vk::ImageSubresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
    };
    if (depth_value) {
        s.cmd.clearDepthStencilImage(image, vk::ImageLayout::eGeneral,
                                     vk::ClearDepthStencilValue{.depth = *depth_value},
                                     range(vk::ImageAspectFlagBits::eDepth));
    }
    if (stencil) {
        s.cmd.clearDepthStencilImage(
            image, vk::ImageLayout::eGeneral,
            vk::ClearDepthStencilValue{.stencil = entry.stencil_clear_value},
            range(vk::ImageAspectFlagBits::eStencil));
    }
    if (color_zero) {
        s.cmd.clearColorImage(image, vk::ImageLayout::eGeneral, vk::ClearColorValue{},
                              range(vk::ImageAspectFlagBits::eColor));
    }
    Written(&entry, entry.rewrite_clock); // NAT-148
    s.buffers_written = true; // the next work waits for the clear
    ++s.commands;
    ++stats.rewrite_clears;
}

void NativeRenderer::ReadFillShader(const Shader::Info& cs, DrawBindings& b) {
    // GT Sport's fill shader reads [lo, hi, count] from its first buffer and writes lo, hi,
    // lo, hi... (count dwords) to its second: a memset of depth buffers, stencil planes and
    // pictures.
    constexpr u64 FillShader = 0x8aa6e00c;
    if (cs.pgm_hash != FillShader || cs.buffers.size() < 2) {
        return;
    }
    const Shader::BufferResource* args = nullptr;
    const Shader::BufferResource* out = nullptr;
    for (const auto& desc : cs.buffers) {
        if (desc.IsSpecial()) {
            continue;
        }
        if (desc.is_written) {
            out = out ? out : &desc;
        } else {
            args = args ? args : &desc;
        }
    }
    if (!args || !out) {
        return;
    }
    const auto args_sharp = args->GetSharp(cs);
    const auto out_sharp = out->GetSharp(cs);
    const void* arguments = reinterpret_cast<const void*>(args_sharp.base_address);
    if (args_sharp.base_address == 0 || !Native::Readable(arguments, 12)) {
        return;
    }
    std::array<u32, 3> words{};
    if (b.fill_arguments_snapshot) {
        words = *b.fill_arguments_snapshot;
    } else {
        std::memcpy(words.data(), arguments, sizeof(words));
    }
    b.fill_pattern = std::array<u32, 2>{words[0], words[1]};
    b.fill_address = out_sharp.base_address;
    b.fill_size = std::min<u64>(u64(words[2]) * 4, out_sharp.GetSize());
    ++stats.fill_shaders;
}

bool NativeRenderer::MetaClearDispatch(const Shader::Info& cs) {
    // As IsComputeMetaClear: a shader writing a known CMASK/HTILE without tile address math
    // (no XOR) and reading none clears it. The dispatch still runs (FIX-112: GT Sport reuses
    // such memory for pictures).
    for (const auto& desc : cs.buffers) {
        if (!desc.IsSpecial() && !desc.is_written &&
            known_meta.contains(desc.GetSharp(cs).base_address)) {
            return false;
        }
    }
    if (cs.has_bitwise_xor) {
        return false;
    }
    bool cleared = false;
    for (const auto& desc : cs.buffers) {
        const VAddr address = desc.GetSharp(cs).base_address;
        if (!desc.IsSpecial() && desc.is_written && known_meta.contains(address)) {
            cleared_meta.insert(address);
            cleared = true;
            if (r.FrameDumpActive()) {
                r.FrameDumpEvent(fmt::format("dispatch: metadata clear {:#x}", address));
            }
        }
    }
    stats.meta_clears += cleared;
    return cleared;
}

void NativeRenderer::DispatchDirect() {
    std::scoped_lock lock{mutex};
    const auto start = std::chrono::steady_clock::now();
    struct Timed {
        std::chrono::steady_clock::time_point start;
        u64& total;
        ~Timed() {
            total += std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
        }
    } timed{start, stats.dispatch_ns};
    SceneTrace::CpuScope scene_trace{r.FrameDumpCollecting(),frame,"dispatch"};
    if(SceneTrace::Id())r.FrameDumpEvent(fmt::format("scene-pass id={} kind=dispatch",SceneTrace::Id()));
    t_timed = false; // OPT-001: the phase timers are the draws'
    r.scheduler.PopPendingOperations();
    const auto& cs_program = r.liverpool->DrawCsRegs();
    const ComputePipeline* pipeline = r.AcquireComputePipeline();
    if (!pipeline) {
        ++stats.no_pipeline;
        return;
    }
    const auto& regs = r.liverpool->DrawRegs();
    MetaClearDispatch(pipeline->GetStage(Shader::SwStage::Compute));
    if (r.FrameDumpActive()) {
        r.FrameDumpDispatch(pipeline->GetStage(Shader::SwStage::Compute), cs_program,
                            false); // NAT-118
    }
    UiHires::Get().scale = 0.f;
    SyncUiReads(pipeline->GetStages());
    Stream& s = Begin();
    DrawBindings& b = bindings;
    b.Clear();
    b.push = UserData(regs);
    BindStages(pipeline->GetStages(), regs, true, b);
    if (b.failed) {
        ++stats.skipped;
        return;
    }
    ReadFillShader(pipeline->GetStage(Shader::SwStage::Compute), b); // NAT-120
    PrepareImages();
    GlobalBarrier();
    // Intrusive opt-in diagnostic. Submit the bound input copy and wait before
    // recording this dispatch; shader and loop bounds are never modified.
    const auto& diag_cs = pipeline->GetStage(Shader::SwStage::Compute);
    static const u64 capture_budget = [] {
        const char* p=std::getenv("RYGT_A82_GPU_CAPTURE");
        return p && *p ? std::min<u64>(std::strtoull(p,nullptr,0),256) : u64{0};
    }();
    static const u64 capture_from = [] {
        const char* p=std::getenv("RYGT_A82_GPU_CAPTURE_FROM_FRAME");
        return p && *p ? std::strtoull(p,nullptr,0) : u64{0};
    }();
    static u64 captures=0;
    const char* forced_descriptor=std::getenv("RYGT_A82D2FE_DESCRIPTOR_ONLY");
    const bool after_race_ready=A82CaptureAfterRace();
    if (after_race_ready && capture_budget && captures < capture_budget && frame >= capture_from &&
        diag_cs.pgm_hash == 0xa82d2fe && forced_descriptor && forced_descriptor[0]=='1' &&
        !diag_cs.buffers.empty() && !diag_cs.buffers[0].IsSpecial() && !b.buffers.empty()) {
        constexpr u64 max_bytes=512;
        const u64 bytes = b.buffers[0].range == VK_WHOLE_SIZE || b.buffers[0].range < b.push.buf_offsets[0]
            ? 0 : std::min<u64>((b.buffers[0].range-b.push.buf_offsets[0]) & ~u64{3},max_bytes);
        const auto descriptor=b.buffers[0];
        const u64 adjust=b.push.buf_offsets[0];
        const bool valid=descriptor.buffer && bytes >= 332 && adjust % 4 == 0 &&
            descriptor.offset <= std::numeric_limits<u64>::max()-adjust-bytes &&
            (descriptor.range != VK_WHOLE_SIZE &&
             (descriptor.range >= adjust && descriptor.range-adjust >= bytes));
        auto readback=valid ? device->CreateBuffer({.size=bytes,
            .usage=Native::Rhi::BufferUsageTransferDst | Native::Rhi::BufferUsageStorage,
            .memory=Native::Rhi::MemoryKind::Readback, .name="a82 input diagnostic"}) : nullptr;
        if (readback && readback->Mapped()) {
            const u64 capture=++captures;
            const auto original_push=b.push;
            std::string push_words;
            std::array<u32,sizeof(original_push)/4> push{};
            std::memcpy(push.data(), &original_push, sizeof(original_push));
            for(u32 word:push) push_words += fmt::format(" {:08x}",word);
            s.cmd.copyBuffer(descriptor.buffer,
                vk::Buffer{Handle<VkBuffer_T>(readback->NativeHandle())},
                vk::BufferCopy{.srcOffset=descriptor.offset+adjust,.dstOffset=0,.size=bytes});
            const vk::MemoryBarrier2 to_host{
                .srcStageMask=vk::PipelineStageFlagBits2::eAllTransfer,
                .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask=vk::AccessFlagBits2::eHostRead};
            s.cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount=1,.pMemoryBarriers=&to_host});
            Splice();
            r.scheduler.Finish();
            readback->Invalidate(0,bytes);
            std::array<u32,128> words{};
            std::memcpy(words.data(),readback->Mapped(),bytes);
            std::string raw; for(u64 i=0;i<bytes/4;++i) raw+=fmt::format(" {:08x}",words[i]);
            LOG_WARNING(Render_Vulkan,
                "DIAG-A82 GPU-PRE-DISPATCH capture={} frame={} groups={}x{}x{} "
                "descriptor={:#x} offset={} range={} adjust={} bytes={} n={} push:{} words:{}",
                capture,frame,cs_program.dim_x,cs_program.dim_y,cs_program.dim_z,
                reinterpret_cast<u64>(static_cast<VkBuffer>(descriptor.buffer)),
                descriptor.offset,descriptor.range,adjust,bytes,words[72],push_words,raw);
            LOG_WARNING(Render_Vulkan,
                "DIAG-A82 GPU-METADATA capture={} local={}x{}x{} lds_per_group={} bound_lds_range={}",
                capture,cs_program.num_thread_x.full,cs_program.num_thread_y.full,
                cs_program.num_thread_z.full,cs_program.SharedMemSize(),
                b.buffers.size()>2 ? b.buffers[2].range : u64{0});
            for(size_t i=0;i<b.images.size();++i) {
                const auto& d=b.images[i];
                LOG_WARNING(Render_Vulkan,"DIAG-A82 GPU-METADATA capture={} bound_image={} view={:#x} sampler={:#x} layout={}",
                    capture,i,reinterpret_cast<u64>(static_cast<VkImageView>(d.imageView)),
                    reinterpret_cast<u64>(static_cast<VkSampler>(d.sampler)),u32(d.imageLayout));
            }
            for(size_t i=0;i<diag_cs.images.size();++i) {
                const auto sharp=diag_cs.images[i].GetSharp(diag_cs);
                std::array<u32,sizeof(sharp)/4> raw_sharp{};
                std::memcpy(raw_sharp.data(),&sharp,sizeof(sharp));
                std::string text;for(u32 word:raw_sharp)text+=fmt::format(" {:08x}",word);
                LOG_WARNING(Render_Vulkan,"DIAG-A82 GPU-METADATA capture={} image_sharp={} words:{}",capture,i,text);
            }
            for(size_t i=0;i<diag_cs.samplers.size();++i) {
                const auto sharp=diag_cs.samplers[i].GetSharp(diag_cs);
                std::array<u32,sizeof(sharp)/4> raw_sharp{};
                std::memcpy(raw_sharp.data(),&sharp,sizeof(sharp));
                std::string text;for(u32 word:raw_sharp)text+=fmt::format(" {:08x}",word);
                LOG_WARNING(Render_Vulkan,"DIAG-A82 GPU-METADATA capture={} sampler_sharp={} words:{}",capture,i,text);
            }
            // Splice retires upload chunks. Rebuild all bindings in a fresh stream;
            // the subsequent descriptor can differ, so this is a pre-rebind snapshot.
            Begin();
            b.Clear();
            b.push=UserData(regs);
            BindStages(pipeline->GetStages(),regs,true,b);
            if (b.failed) { ++stats.skipped; return; }
            PrepareImages();
            GlobalBarrier();
            const auto rebound=b.buffers.empty()?vk::DescriptorBufferInfo{}:b.buffers[0];
            // Only replace when the entire original logical descriptor was copied.
            // This does not rely on the hash identifying a particular SPIR-V variant.
            const char* freeze_env=std::getenv("RYGT_A82_FREEZE_CAPTURE");
            const bool frozen=freeze_env && freeze_env[0]=='1' && !b.buffers.empty() &&
                !diag_cs.buffers[0].is_written && descriptor.range-adjust == bytes;
            if (frozen) {
                b.buffers[0]={Handle<VkBuffer_T>(readback->NativeHandle()),0,bytes};
                b.push.buf_offsets[0]=0;
                s.keep.push_back(std::move(readback)); // retained until this command list completes
            }
            std::array<u32,sizeof(b.push)/4> effective_push{};
            std::memcpy(effective_push.data(),&b.push,sizeof(b.push));
            std::string effective;for(u32 word:effective_push)effective+=fmt::format(" {:08x}",word);
            LOG_WARNING(Render_Vulkan,
                "DIAG-A82 POST-REBIND capture={} descriptor={:#x} offset={} range={} adjust={} frozen={} push:{}",
                capture,reinterpret_cast<u64>(static_cast<VkBuffer>(rebound.buffer)),
                rebound.offset,rebound.range,b.push.buf_offsets[0],frozen,effective);
        } else {
            LOG_WARNING(Render_Vulkan,"DIAG-A82 GPU input capture skipped: invalid descriptor or readback allocation");
            captures=capture_budget; // avoid repeating a failed diagnostic every dispatch
        }
    }
    // Wait for an existing non-rendering boundary: never restart a clear pass for diagnostics.
    if(SceneTrace::Id() && !SceneTrace::state.attempted && !s.rendering) {
        const auto families=r.instance.GetPhysicalDevice().getQueueFamilyProperties();
        const u32 family=r.instance.GetGraphicsQueueFamilyIndex();
        SceneTrace::Prepare(r.instance.GetDevice(),s.cmd,
            family<families.size()?families[family].timestampValidBits:0,
            r.instance.GetPhysicalDevice().getProperties().limits.timestampPeriod);
    }
    scene_trace.Programs(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash);
    if(SceneTrace::Id())r.FrameDumpEvent(fmt::format("scene-compute id={} cs={:#x} groups={}x{}x{} threads={}x{}x{} host_subgroup={}",SceneTrace::Id(),pipeline->GetStage(Shader::SwStage::Compute).pgm_hash,cs_program.dim_x,cs_program.dim_y,cs_program.dim_z,cs_program.num_thread_x.full,cs_program.num_thread_y.full,cs_program.num_thread_z.full,r.instance.SubgroupSize()));
    SceneTrace::GpuBegin(s.cmd);
    const CmdBuf cmdbuf{s.cmd};
    bool transferred = false;
    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    if (b.fill_arguments_snapshot && b.fill_pattern && b.fill_size != 0 &&
        cs.pgm_hash == 0x8aa6e00c && cs.flattened_ud_buf.size() > 2 &&
        cs_program.dim_y == 1 && cs_program.dim_z == 1 &&
        cs_program.start_x == 0 && cs_program.start_y == 0 && cs_program.start_z == 0 &&
        cs_program.num_thread_x.full == 256 && cs_program.num_thread_y.full == 1 &&
        cs_program.num_thread_z.full == 1) {
        const auto& words = *b.fill_arguments_snapshot;
        // This shader writes two 256-word halves per group. Require the complete
        // range, with equal argument/output bounds. Two-word patterns use the
        // existing bounded GPU pattern cache; no fill or invalidation is omitted.
        if (words[2] == cs.flattened_ud_buf[2] &&
            b.fill_size == u64(words[2]) * 4 && u64(cs_program.dim_x) * 512 >= words[2] &&
            (b.fill_address & 3) == 0 && RegionOver(b.fill_address, b.fill_size)) {
            if (const auto target = GuestBuffer(b.fill_address, b.fill_size, true);
                target && (target->second & 3) == 0) {
                if (words[0] == words[1]) {
                    cmdbuf.fillBuffer(target->first, target->second, b.fill_size, words[0]);
                    transferred = true;
                } else if (const auto* pattern = PatternBuffer(words[0], words[1], b.fill_size);
                           pattern && pattern->Size() >= b.fill_size) {
                    cmdbuf.copyBuffer(vk::Buffer{Handle<VkBuffer_T>(pattern->NativeHandle())},
                                     target->first, vk::BufferCopy{.srcOffset = 0,
                                         .dstOffset = target->second, .size = b.fill_size});
                    transferred = true;
                }
            }
        }
    }
    if (!transferred) {
        // FIX-188: Intel Arc (#65) binds the pipeline every time (tile work runs between).
        s.pipelines.Compute(pipeline->Handle(), r.instance.IsIntel(), [&] {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
        });
        // CPU guest bytes may differ from the GPU descriptor; label accordingly.
        static const bool capture_a82 = [] {
            const char* p = std::getenv("RYGT_A82_INPUT_DIAG"); return p && p[0] == '1';
        }();
        if (capture_a82 && cs.pgm_hash == 0xa82d2fe && !cs.buffers.empty() &&
            !cs.buffers[0].IsSpecial() && !b.buffers.empty()) {
            const auto sharp = cs.buffers[0].GetSharp(cs);
            const u64 base = sharp.base_address, size = sharp.GetSize();
            std::array<u32, 80> words{};
            const bool readable = base && size >= sizeof(words) &&
                Core::RytyMemory::GuardedCopy(words.data(),
                    reinterpret_cast<const void*>(base), sizeof(words));
            static u64 serial = 0;
            static u32 last_n = ~u32{0};
            static u64 last_base = ~u64{0};
            static bool last_readable = false;
            const u64 number = ++serial;
            const u32 n = readable ? words[72] : 0;
            if (number <= 4 || number % 300 == 0 || base != last_base ||
                readable != last_readable || (readable && n != last_n)) {
                last_n = n; last_base = base; last_readable = readable;
                const auto& descriptor = b.buffers[0];
                std::string raw;
                if (readable) for (u32 word : words) raw += fmt::format(" {:08x}", word);
                LOG_WARNING(Render_Vulkan,
                    "DIAG-A82 CPU-GUEST-NOT-GPU frame={} serial={} groups={}x{}x{} "
                    "buffer0={:#x}+{:#x} region={} readable={} n={} gpu_buffer={:#x} "
                    "gpu_offset={} gpu_range={} push_offset={} words:{}",
                    frame, number, cs_program.dim_x, cs_program.dim_y, cs_program.dim_z,
                    base, size, RegionOver(base, size) != nullptr, readable, n,
                    reinterpret_cast<u64>(static_cast<VkBuffer>(descriptor.buffer)),
                    descriptor.offset, descriptor.range, b.push.buf_offsets[0], raw);
                for (size_t i=0; i<b.images.size(); ++i) {
                    const auto& d = b.images[i];
                    LOG_WARNING(Render_Vulkan, "DIAG-A82 bound_image={} view={:#x} sampler={:#x} layout={}",
                        i, reinterpret_cast<u64>(static_cast<VkImageView>(d.imageView)),
                        reinterpret_cast<u64>(static_cast<VkSampler>(d.sampler)), u32(d.imageLayout));
                }
                for (size_t i=0; i<cs.samplers.size(); ++i) {
                    const auto sharp_sampler=cs.samplers[i].GetSharp(cs);
                    std::array<u32, sizeof(sharp_sampler)/sizeof(u32)> data{};
                    std::memcpy(data.data(), &sharp_sampler, sizeof(sharp_sampler));
                    std::string sampler_words;
                    for (u32 word : data) sampler_words += fmt::format(" {:08x}", word);
                    LOG_WARNING(Render_Vulkan, "DIAG-A82 sampler={} words:{}", i, sampler_words);
                }
                for (size_t i=0; i<cs.images.size(); ++i) {
                    const auto image = cs.images[i].GetSharp(cs);
                    LOG_WARNING(Render_Vulkan,
                        "DIAG-A82 image={} guest={:#x} {}x{} layers={}-{} type={} fmt={}/{}",
                        i, image.Address(), image.width+1, image.height+1,
                        u32(image.base_array), u32(image.last_array), u32(image.GetType()),
                        u32(image.GetDataFmt()), u32(image.GetNumberFmt()));
                }
            }
        }
        // Opt-in: when buffer0 is an upload chunk, its already-copied bytes are
        // the bound snapshot, unlike a second read of mutable guest memory.
        static const bool inspect_snapshot=[] {
            const char* p=std::getenv("RYGT_A82_INPUT_DIAG"); return p && p[0]=='1';
        }();
        if (inspect_snapshot && cs.pgm_hash==0xa82d2fe && !b.buffers.empty()) {
            constexpr u64 wanted=336;
            const auto info=b.buffers[0]; const u64 adjust=b.push.buf_offsets[0];
            bool found=false;
            for (const auto& chunk:s.chunks) {
                if (!chunk.buffer || !chunk.buffer->Mapped() ||
                    vk::Buffer{Handle<VkBuffer_T>(chunk.buffer->NativeHandle())}!=info.buffer ||
                    info.offset>chunk.buffer->Size() || adjust>chunk.buffer->Size()-info.offset ||
                    wanted>chunk.buffer->Size()-info.offset-adjust ||
                    info.range==VK_WHOLE_SIZE || info.range<adjust || wanted>info.range-adjust) continue;
                std::array<u32,84> copied{};
                std::memcpy(copied.data(),chunk.buffer->Mapped()+info.offset+adjust,wanted);
                static u64 serial=0;static u32 last=~u32{0};static u64 last_base=~u64{0};
                const u64 number=++serial;
                const u64 base=cs.buffers.empty() || cs.buffers[0].IsSpecial()
                    ? 0 : cs.buffers[0].GetSharp(cs).base_address;
                if (number<=4 || number%300==0 || copied[72]!=last || base!=last_base) {
                    last=copied[72];last_base=base;std::string raw;
                    for(u32 word:copied)raw+=fmt::format(" {:08x}",word);
                    LOG_WARNING(Render_Vulkan,
                        "DIAG-A82 UPLOAD-SNAPSHOT frame={} serial={} guest={:#x} n={} buffer={:#x} offset={} adjust={} words:{}",
                        frame,number,base,copied[72],reinterpret_cast<u64>(static_cast<VkBuffer>(info.buffer)),
                        info.offset,adjust,raw);
                }
                found=true;break;
            }
            if (!found) {
                static std::atomic<u64> missing{};const auto n=++missing;
                if(n<=4 || n%300==0)LOG_WARNING(Render_Vulkan,
                    "DIAG-A82 UPLOAD-SNAPSHOT-UNAVAILABLE frame={} count={} descriptor={:#x}",
                    frame,n,reinterpret_cast<u64>(static_cast<VkBuffer>(info.buffer)));
            }
        }
        PushBindings(*pipeline, vk::PipelineBindPoint::eCompute, b);
        Native::GpuCheckpoints::Record(static_cast<VkCommandBuffer>(s.cmd), 2, frame,
            cs.pgm_hash, cs_program.dim_x, u64(cs_program.dim_y) << 32 | cs_program.dim_z);
        cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    }
    SceneTrace::GpuEnd(s.cmd);
    if(SceneTrace::Id())r.FrameDumpEvent(fmt::format("scene-compute-path id={} transfer_rewrite={}",SceneTrace::Id(),transferred));
    NoteWrites(nullptr, b);
    if (Native::g_dump_request.exchange(false)) {
        r.StartFrameDumpNow(); // FIX-156 (diagnosis): a watched fill
    }
    s.buffers_written = true; // what the dispatch wrote is seen after the next barrier
    ++stats.dispatches;
    ++s.commands;
    ChunkFlushCheck();
    Report();
}

void NativeRenderer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    std::scoped_lock lock{mutex};
    SceneTrace::CpuScope scene_trace{r.FrameDumpCollecting(),frame,"dispatch-indirect"};
    if(SceneTrace::Id())r.FrameDumpEvent(fmt::format("scene-pass id={} kind=dispatch-indirect arguments={:#x}+{} bytes={}",SceneTrace::Id(),address,offset,size));
    t_timed = false; // OPT-001
    r.scheduler.PopPendingOperations();
    const ComputePipeline* pipeline = r.AcquireComputePipeline();
    if (!pipeline) {
        ++stats.no_pipeline;
        return;
    }
    const auto& regs = r.liverpool->DrawRegs();
    if (r.FrameDumpActive()) {
        r.FrameDumpDispatch(pipeline->GetStage(Shader::SwStage::Compute),
                            r.liverpool->DrawCsRegs(), true); // NAT-118
    }
    UiHires::Get().scale = 0.f;
    SyncUiReads(pipeline->GetStages());
    Stream& s = Begin();
    const auto args = GuestBuffer(address + offset, std::max<u32>(size, 12), false);
    if (!args) {
        ++stats.skipped;
        return;
    }
    DrawBindings& b = bindings;
    b.Clear();
    b.push = UserData(regs);
    BindStages(pipeline->GetStages(), regs, true, b);
    PrepareImages();
    GlobalBarrier();
    const CmdBuf cmdbuf{s.cmd};
    // FIX-188: Intel Arc (#65) binds the pipeline every time (tile work runs between).
    s.pipelines.Compute(pipeline->Handle(), r.instance.IsIntel(), [&] {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    });
    PushBindings(*pipeline, vk::PipelineBindPoint::eCompute, b);
    // Wait for an existing non-rendering boundary: never restart a clear pass for diagnostics.
    if(SceneTrace::Id() && !SceneTrace::state.attempted && !s.rendering) {
        const auto families=r.instance.GetPhysicalDevice().getQueueFamilyProperties();
        const u32 family=r.instance.GetGraphicsQueueFamilyIndex();
        SceneTrace::Prepare(r.instance.GetDevice(),s.cmd,
            family<families.size()?families[family].timestampValidBits:0,
            r.instance.GetPhysicalDevice().getProperties().limits.timestampPeriod);
    }
    scene_trace.Programs(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash);
    if(SceneTrace::Id() && scene_indirect_args.size()<128) {
        if(!scene_indirect_buffer)scene_indirect_buffer=device->CreateBuffer({.size=128*16,.usage=Native::Rhi::BufferUsageTransferDst,.memory=Native::Rhi::MemoryKind::Readback,.name="scene indirect arguments"});
        if(scene_indirect_buffer && scene_indirect_buffer->Mapped()){
            const u64 target=scene_indirect_args.size()*16;
            s.cmd.copyBuffer(args->first,vk::Buffer{Handle<VkBuffer_T>(scene_indirect_buffer->NativeHandle())},vk::BufferCopy{.srcOffset=args->second,.dstOffset=target,.size=12});
            scene_indirect_args.push_back({SceneTrace::Id(),address,offset});
        } else r.FrameDumpEvent(fmt::format("scene-indirect-snapshot id={} skipped=allocation",SceneTrace::Id()));
    }else if(SceneTrace::Id())r.FrameDumpEvent(fmt::format("scene-indirect-snapshot id={} skipped=capacity",SceneTrace::Id()));
    SceneTrace::GpuBegin(s.cmd);
    cmdbuf.dispatchIndirect(args->first, args->second);
    SceneTrace::GpuEnd(s.cmd);
    NoteWrites(nullptr, b);
    s.buffers_written = true;
    ++stats.dispatches;
    ++s.commands;
    ChunkFlushCheck();
}

void NativeRenderer::ResolveDraw(const AmdGpu::Regs& regs) {
    if (UiHires::Enabled()) {
        ResolveUiDisplay(regs.color_buffers[0].Address(), true);
        ResolveUiDisplay(regs.color_buffers[1].Address(), false);
    }
    const VideoCore::TextureCache::ImageDesc src_desc{regs.color_buffers[0],
                                                      r.liverpool->DrawCbExtent(0)};
    const VideoCore::TextureCache::ImageDesc dst_desc{regs.color_buffers[1],
                                                      r.liverpool->DrawCbExtent(1)};
    Native::ImageEntry* src = registry->Find(src_desc.info, true, frame);
    Native::ImageEntry* dst =
        registry->Find(dst_desc.info, true, frame, true, src_desc.info.num_samples > 1);
    if (!src || !dst) {
        return;
    }
    Stream& s = Begin();
    // NAT-122: a fill before the resolve is applied before it (left pending, it cleared the
    // resolved picture at its first read: GT Sport's minimaps, resolved into the scratch pool).
    ApplyRewrite(*src);
    ApplyRewrite(*dst);
    if (r.FrameDumpActive()) {
        r.FrameDumpEvent(fmt::format("resolve {:#x} {}x{} x{} -> {:#x} {}x{} x{}",
                                     src->info.guest_address, src->info.size.width,
                                     src->info.size.height, src->info.num_samples,
                                     dst->info.guest_address, dst->info.size.width,
                                     dst->info.size.height, dst->info.num_samples));
        if (r.FrameDumpCollecting() &&
            std::ranges::find(dump_entries, dst) == dump_entries.end()) {
            dump_entries.push_back(dst);
        }
    }
    PrepareImages();
    GlobalBarrier();
    const auto layers = std::min(src_desc.view_info.range.extent.layers,
                                 dst_desc.view_info.range.extent.layers);
    const vk::ImageResolve region{
        .srcSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = src_desc.view_info.range.base.layer,
                        .layerCount = layers},
        .dstSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = dst_desc.view_info.range.base.layer,
                        .layerCount = layers},
        // FIX-154: the targets the resolve names (they may share larger pictures).
        .extent{std::min({src->info.size.width, dst->info.size.width, src_desc.info.size.width,
                          dst_desc.info.size.width}),
                std::min({src->info.size.height, dst->info.size.height,
                          src_desc.info.size.height, dst_desc.info.size.height}),
                1},
    };
    const vk::Image src_image{Handle<VkImage_T>(src->image->NativeHandle())};
    const vk::Image dst_image{Handle<VkImage_T>(dst->image->NativeHandle())};
    if (src->image->Desc().samples > 1 && dst->image->Desc().samples == 1) {
        CmdBuf{s.cmd}.resolveImage(src_image, vk::ImageLayout::eGeneral, dst_image,
                                   vk::ImageLayout::eGeneral, region);
    } else {
        const vk::ImageCopy copy{
            .srcSubresource = region.srcSubresource,
            .dstSubresource = region.dstSubresource,
            .extent = region.extent,
        };
        CmdBuf{s.cmd}.copyImage(src_image, vk::ImageLayout::eGeneral, dst_image,
                                vk::ImageLayout::eGeneral, copy);
    }
    Written(dst);
    s.buffers_written = true;
    ++s.commands;
}

void NativeRenderer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    if (num_bytes == 0) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (!is_gds) {
        SyncUiRange(address, num_bytes, false);
    }
    Stream& s = Begin();
    if (is_gds) {
        GlobalBarrier();
        CmdBuf{s.cmd}.fillBuffer(vk::Buffer{Handle<VkBuffer_T>(gds->NativeHandle())}, address,
                                 std::min<u64>(num_bytes, gds->Size() - address), value);
        s.buffers_written = true;
        ++s.commands;
        return;
    }
    const bool meta = known_meta.contains(address);
    if (meta) {
        cleared_meta.insert(address); // a CMASK/HTILE cleared by the CP
        ++stats.meta_clears;
    }
    if (r.FrameDumpActive()) {
        r.FrameDumpEvent(fmt::format("fill {:#x}+{:#x} = {:#x}{}", address, num_bytes, value,
                                     meta ? " (metadata clear)" : "")); // NAT-117
    }
    ++stats.fills;
    if (const auto found = GuestBuffer(address, num_bytes, true)) {
        GlobalBarrier();
        CmdBuf{s.cmd}.fillBuffer(found->first, found->second, num_bytes, value);
        s.buffers_written = true;
        ++s.commands;
    } else {
        // Not GPU-visible as one range: written now (before the GPU runs what precedes it).
        if (r.memory->IsValidMapping(address, num_bytes)) {
            std::fill_n(reinterpret_cast<u32*>(address), num_bytes / 4, value);
        }
    }
    const std::array<u32, 2> pattern{value, value};
    GpuWrote(address, num_bytes, &pattern, true); // NAT-119/120
}

void NativeRenderer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    if (num_bytes == 0) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (!src_gds) {
        SyncUiRange(src, num_bytes, true);
    }
    if (!dst_gds) {
        SyncUiRange(dst, num_bytes, false);
    }
    Stream& s = Begin();
    ++stats.copies;
    if (r.FrameDumpActive()) {
        r.FrameDumpEvent(fmt::format("copy {:#x}{} <- {:#x}{} ({:#x} bytes)", dst,
                                     dst_gds ? " (GDS)" : "", src, src_gds ? " (GDS)" : "",
                                     num_bytes)); // NAT-117
    }
    const vk::Buffer gds_buffer{Handle<VkBuffer_T>(gds->NativeHandle())};
    const auto source = src_gds ? std::optional{std::pair{gds_buffer, u64(src)}}
                                : GuestBuffer(src, num_bytes, false);
    const auto dest = dst_gds ? std::optional{std::pair{gds_buffer, u64(dst)}}
                              : GuestBuffer(dst, num_bytes, true);
    if (source && dest) {
        GlobalBarrier();
        CmdBuf{s.cmd}.copyBuffer(source->first, dest->first,
                                 vk::BufferCopy{.srcOffset = source->second,
                                                .dstOffset = dest->second,
                                                .size = num_bytes});
        s.buffers_written = true;
        ++s.commands;
    } else if (!dst_gds && !src_gds) {
        if (r.memory->IsValidMapping(dst, num_bytes) &&
            Native::Readable(reinterpret_cast<const void*>(src), num_bytes)) {
            std::memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src),
                        num_bytes);
        }
    } else {
        static std::atomic<u32> logged{};
        if (Sample(logged)) {
            LOG_WARNING(Render_Vulkan, "NAT-101: GDS copy {:#x} <- {:#x} ({} bytes) dropped", dst,
                        src, num_bytes);
        }
    }
    if (!dst_gds) {
        GpuWrote(dst, num_bytes, nullptr, true); // NAT-119
    }
}

bool NativeRenderer::InlineWrite(VAddr address, std::span<const u8> data) {
    if (data.empty() || data.size() % 4 != 0 || data.size() > 65536 || address % 4 != 0) {
        return false;
    }
    std::scoped_lock lock{mutex};
    auto* direct = r.buffer_cache.GetDirectMemory();
    if (!direct || !direct->EnsureImported(address, data.size())) {
        return false;
    }
    const auto found = direct->FindBuffer(address, data.size());
    if (!found) {
        return false;
    }
    Stream& s = Begin();
    GlobalBarrier();
    CmdBuf{s.cmd}.updateBuffer(found->first->Handle(), found->second, data.size(), data.data());
    // NAT-146: a region over the range holds what the GPU reads there: it gets the bytes too.
    for (auto& [base, region] : regions) {
        const VAddr begin = std::max<VAddr>(address, region.begin);
        const VAddr end = std::min<VAddr>(address + data.size(), region.end);
        if (begin < end) {
            CmdBuf{s.cmd}.updateBuffer(vk::Buffer{Handle<VkBuffer_T>(region.buffer->NativeHandle())},
                                       begin - region.begin, end - begin,
                                       data.data() + (begin - address));
        }
    }
    // NAT-150: and mirrors.
    for (auto it = FirstMirror(address);
         it != mirrors.end() && it->second.begin < address + data.size(); ++it) {
        const Mirror& mirror = it->second;
        const VAddr begin = std::max<VAddr>(address, mirror.begin);
        const VAddr end = std::min<VAddr>(address + data.size(), mirror.end);
        CmdBuf{s.cmd}.updateBuffer(vk::Buffer{Handle<VkBuffer_T>(mirror.buffer->NativeHandle())},
                                   begin - mirror.begin, end - begin,
                                   data.data() + (begin - address));
        for (u64 g = (begin - mirror.begin) / MirrorGranule;
             g <= (end - 1 - mirror.begin) / MirrorGranule && g < mirror.hashes.size(); ++g) {
            const_cast<Mirror&>(mirror).hashes[g] = 0; // FIX-176
        }
    }
    s.buffers_written = true;
    ++s.commands;
    ++stats.inline_writes;
    return true;
}

void NativeRenderer::OnUnmap(VAddr address, u64 size) {
    if (UiHires::Enabled()) {
        auto& ui = UiHires::Get();
        std::scoped_lock lock{ui.mutex};
        for (auto it = ui.proxies.begin(); it != ui.proxies.end();) {
            const auto base = it->first;
            const u64 bytes = u64(it->second.native.width) * it->second.native.height * 4;
            if (size != 0 && ((base >= address && base - address < size) ||
                              (address >= base && address - base < bytes))) {
                UiHires::MarkStale(it->second, false);
                UiHires::Retire(ui, it->second);
                for (auto& known : ui.addresses) {
                    VAddr expected = base;
                    known.compare_exchange_strong(expected, 0);
                }
                it = ui.proxies.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (Native::WatchedTexture(address, address + size)) {
        LOG_WARNING(Render_Vulkan, "FIX-153 watch: unmap {:#x}+{:#x}", address, size);
    }
    Native::ForgetReadable(address, size); // NAT-147
    // FIX-153: the pages stop being watched (Ryty tells before it unmaps them). Memory mapped
    // there again comes writable; a watch left in place counted as one in the page manager but
    // protected nothing, and was never set again (the page was "watched"). GT Sport maps a car's
    // models and textures into the same memory each time: the showroom's 32x32 lighting texture
    // kept the zeros of the fresh mapping, and the car screens came out blown out.
    if (size != 0) {
        UnwatchPages(VideoCore::PageManager::GetPageAddr(address),
                     VideoCore::PageManager::GetNextPageAddr(address + size - 1));
    }
    registry->Invalidate(address, size, Native::ImageRegistry::Stale::Unmap);
    // NAT-146: regions over unmapped memory go (the device frees their buffers once the GPU is
    // done with them).
    std::scoped_lock lock{mutex};
    {
        std::unique_lock map_lock{buffer_map_mutex}; // OPT-010
        for (auto it = regions.begin(); it != regions.end();) {
            if (it->second.begin < address + size && it->second.end > address) {
                buffer_log.Note(it->second.begin, it->second.end);
                retired_watch_ranges.emplace_back(it->second.begin, it->second.end);
                it = regions.erase(it);
            } else {
                ++it;
            }
        }
    }
    DropMirrors(address, size); // NAT-150
    {
        std::scoped_lock pending_lock{region_dirty_mutex}; // FIX-173
        std::erase_if(page_snapshots, [&](const auto& snapshot) {
            return snapshot.first >= address && snapshot.first < address + size;
        });
    }
    ++buffer_generation;        // OPT-003
    ++unmap_generation;         // OPT-002
}

void NativeRenderer::WatchBind(const Native::ImageEntry& entry, const AmdGpu::Image& tsharp,
                               bool storage, bool memo, u64 program) {
    if (!Native::WatchSample()) {
        return;
    }
    u32 sources = 0;
    {
        std::scoped_lock lock{const_cast<Native::ImageEntry&>(entry).sources_mutex};
        sources = static_cast<u32>(entry.sources.size());
    }
    LOG_WARNING(Render_Vulkan, "FIX-157 watch: {} {:#x} {}x{} layers {}-{} type {} fmt {}/{}{} "
                "(program {:#x}) -> {} {}x{} layers {} version {}, {} sources (frame {})",
                storage ? "store" : "sample", tsharp.Address(), tsharp.width + 1,
                tsharp.height + 1, u32(tsharp.base_array), u32(tsharp.last_array),
                u32(tsharp.GetType()), u32(tsharp.GetDataFmt()), u32(tsharp.GetNumberFmt()),
                memo ? " (memo)" : "", program, entry.from_cpu ? "texture" : "target",
                entry.info.size.width, entry.info.size.height, entry.info.resources.layers,
                entry.write_version.load(), sources, frame);
}

void NativeRenderer::ProbeWatchedPictures() {
    // FIX-156 (diagnosis, RYGT_NATIVE_WATCH_TEXTURE): what the pictures at the watched address
    // hold, every half second (read back from the GPU), to see when one loses its content.
    const auto& watched_list = Native::WatchedTextures();
    if (watched_list.empty()) {
        return;
    }
    const u64 done = r.scheduler.GetWorkSemaphore()->KnownGpuTick();
    for (auto it = probes.begin(); it != probes.end();) {
        if (it->tick > done) {
            ++it;
            continue;
        }
        it->buffer->Invalidate(0, it->size);
        const u8* bytes = it->buffer->Mapped();
        const u64 count = it->size / 4;
        double sum = 0;
        float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
        u64 zeros = 0;
        // FIX-171: B10G11R11 pictures (GT Sport's light cubes) as their mean colour channel.
        const auto small_float = [](u32 bits, u32 mantissa_bits) {
            const u32 e = bits >> mantissa_bits;
            const float m = float(bits & ((1u << mantissa_bits) - 1)) / float(1u << mantissa_bits);
            return e == 0 ? std::ldexp(m, -14) : e == 31 ? 0.0f : std::ldexp(1.0f + m, int(e) - 15);
        };
        for (u64 i = 0; i < count; ++i) {
            float v;
            std::memcpy(&v, bytes + i * 4, 4);
            if (it->packed_r11g11b10) {
                u32 word;
                std::memcpy(&word, bytes + i * 4, 4);
                v = (small_float(word & 0x7ff, 6) + small_float((word >> 11) & 0x7ff, 6) +
                     small_float(word >> 22, 5)) / 3.0f;
            }
            if (!std::isfinite(v)) {
                continue;
            }
            sum += v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            zeros += v == 0.0f ? 1 : 0;
        }
        if (it->packed_r11g11b10 && it->layers != 0) {
            // FIX-171: per mip, the mean of each layer (untiled copy, mip after mip).
            std::string text;
            u64 at = 0;
            for (u32 mip = 0; mip < it->mips; ++mip) {
                const u64 w = std::max(it->width >> mip, 1u), h = std::max(it->height >> mip, 1u);
                text += fmt::format(" m{}:", mip);
                for (u32 layer = 0; layer < it->layers; ++layer) {
                    double layer_sum = 0;
                    for (u64 i = 0; i < w * h && (at + i + 1) * 4 <= it->size; ++i) {
                        u32 word;
                        std::memcpy(&word, bytes + (at + i) * 4, 4);
                        layer_sum += (small_float(word & 0x7ff, 6) +
                                      small_float((word >> 11) & 0x7ff, 6) +
                                      small_float(word >> 22, 5)) / 3.0f;
                    }
                    at += w * h;
                    text += fmt::format(" {:.3f}", layer_sum / double(w * h));
                }
            }
            if (it->layers == 1 && it->height >= 3) {
                // A plain picture (the frame's main view): its top, middle and bottom thirds.
                const u64 w = it->width, third = it->height / 3;
                std::array<double, 3> part{};
                for (u32 band = 0; band < 3; ++band) {
                    for (u64 y = band * third; y < (band + 1) * third; ++y) {
                        for (u64 x = 0; x < w; ++x) {
                            u32 word;
                            std::memcpy(&word, bytes + (y * w + x) * 4, 4);
                            part[band] += (small_float(word & 0x7ff, 6) +
                                           small_float((word >> 11) & 0x7ff, 6) +
                                           small_float(word >> 22, 5)) / 3.0f;
                        }
                    }
                    part[band] /= double(third * w);
                }
                text += fmt::format(" thirds: {:.4f} {:.4f} {:.4f}", part[0], part[1], part[2]);
            }
            LOG_WARNING(Render_Vulkan, "FIX-171 watch: frame {}: {} {:#x} {}x{} version {}:{}",
                        it->frame, it->target ? "target" : "texture", it->address, it->width,
                        it->height, it->version, text);
            it = probes.erase(it);
            continue;
        }
        LOG_WARNING(Render_Vulkan, "FIX-156 watch: frame {}: {} {:#x} {}x{} version {} holds "
                    "mean {:.4f} min {:.4f} max {:.4f}, {} of {} words zero (as 32-bit floats)",
                    it->frame, it->target ? "target" : "texture", it->address, it->width,
                    it->height,
                    it->version, count ? sum / count : 0.0, lo, hi, zeros, count);
        it = probes.erase(it);
    }
    // FIX-171 (diagnosis, RYGT_NATIVE_WATCH_EVERY=<frames>): how often (every 30 frames).
    static const u64 every = [] {
        const char* env = std::getenv("RYGT_NATIVE_WATCH_EVERY");
        return env && *env ? std::max<u64>(std::strtoull(env, nullptr, 0), 1) : u64{30};
    }();
    if (frame % every != 0 || probes.size() >= (every >= 30 ? 1u : 16u)) {
        return;
    }
    Begin();
    for (const VAddr watched : watched_list) {
        registry->ForEachOverlapping(watched, watched + 1, [&](Native::ImageEntry& entry) {
            if (entry.info.guest_address != watched || !entry.image ||
                entry.info.num_samples != 1) {
                return;
            }
            const bool packed = entry.info.pixel_format == vk::Format::eB10G11R11UfloatPack32;
            const auto& desc = entry.image->Desc();
            // FIX-171 (RYGT_NATIVE_WATCH_RECT=x,y,w,h): of a plain picture only that rectangle.
            static const std::array<u32, 4> rect = [] {
                std::array<u32, 4> r{};
                if (const char* env = std::getenv("RYGT_NATIVE_WATCH_RECT"); env && *env) {
                    std::sscanf(env, "%u,%u,%u,%u", &r[0], &r[1], &r[2], &r[3]);
                }
                return r;
            }();
            const bool in_rect = packed && rect[2] != 0 && rect[3] != 0 && desc.mips == 1 &&
                                 desc.layers <= 1 && rect[0] + rect[2] <= desc.width &&
                                 rect[1] + rect[3] <= desc.height;
            u64 untiled = 0;
            for (u32 mip = 0; mip < desc.mips; ++mip) {
                untiled += u64(std::max(desc.width >> mip, 1u)) *
                           std::max(desc.height >> mip, 1u) * std::max(desc.layers, 1u) * 4;
            }
            if (in_rect) {
                untiled = u64(rect[2]) * rect[3] * 4;
            }
            const u64 size = packed ? untiled : entry.info.guest_size;
            auto buffer = device->CreateBuffer({.size = size,
                                                .usage = Native::Rhi::BufferUsageStorage |
                                                         Native::Rhi::BufferUsageTransferDst,
                                                .memory = Native::Rhi::MemoryKind::Readback,
                                                .name = "watch probe"});
            if (!buffer || !buffer->Mapped()) {
                return;
            }
            GlobalBarrier();
            if (in_rect) {
                const vk::BufferImageCopy region{
                    .bufferOffset = 0,
                    .imageSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor,
                                      .mipLevel = 0,
                                      .baseArrayLayer = 0,
                                      .layerCount = 1},
                    .imageOffset = {s32(rect[0]), s32(rect[1]), 0},
                    .imageExtent = {rect[2], rect[3], 1},
                };
                stream.cmd.copyImageToBuffer(
                    vk::Image{Handle<VkImage_T>(entry.image->NativeHandle())},
                    vk::ImageLayout::eGeneral,
                    vk::Buffer{Handle<VkBuffer_T>(buffer->NativeHandle())}, region);
                stream.list->Barrier(Native::Rhi::ScopeTransfer, Native::Rhi::ScopeHost);
                probes.push_back({std::move(buffer), r.scheduler.CurrentTick() + 1, size, frame,
                                  entry.write_version.load(), rect[2], rect[3], !entry.from_cpu,
                                  watched, true, 1, 1});
            } else if (packed) {
                // FIX-171: each mip with all its layers, untiled.
                std::vector<vk::BufferImageCopy> regions;
                u64 offset = 0;
                for (u32 mip = 0; mip < desc.mips; ++mip) {
                    const u32 w = std::max(desc.width >> mip, 1u);
                    const u32 h = std::max(desc.height >> mip, 1u);
                    regions.push_back(vk::BufferImageCopy{
                        .bufferOffset = offset,
                        .imageSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor,
                                          .mipLevel = mip,
                                          .baseArrayLayer = 0,
                                          .layerCount = std::max(desc.layers, 1u)},
                        .imageExtent = {w, h, 1},
                    });
                    offset += u64(w) * h * std::max(desc.layers, 1u) * 4;
                }
                stream.cmd.copyImageToBuffer(
                    vk::Image{Handle<VkImage_T>(entry.image->NativeHandle())},
                    vk::ImageLayout::eGeneral,
                    vk::Buffer{Handle<VkBuffer_T>(buffer->NativeHandle())}, regions);
                stream.list->Barrier(Native::Rhi::ScopeTransfer, Native::Rhi::ScopeHost);
                probes.push_back({std::move(buffer), r.scheduler.CurrentTick() + 1, size, frame,
                                  entry.write_version.load(), desc.width, desc.height,
                                  !entry.from_cpu, watched, true, desc.mips,
                                  std::max(desc.layers, 1u)});
            } else {
                auto copies = Native::MipCopies(entry.info);
                stream.list->CopyImageToBuffer(entry.image.get(), buffer.get(), copies);
                stream.list->Barrier(Native::Rhi::ScopeTransfer, Native::Rhi::ScopeHost);
                stream.pipelines.Invalidate();
                probes.push_back({std::move(buffer), r.scheduler.CurrentTick() + 1, size, frame,
                                  entry.write_version.load(), entry.info.size.width,
                                  entry.info.size.height, !entry.from_cpu, watched, false});
            }
        });
    }
}

void NativeRenderer::EvictImages() {
    Native::SlowCallTrace<3> trace{true, "frame-retirement",
        {"completed-views", "cold-mirrors", "cold-images"}};
    ProbeWatchedPictures(); // FIX-156 (diagnosis)
    // NAT-108: images not used for two seconds go (GT Sport's scratch pool holds render
    // targets of ever-changing sizes: the UI's blur panels, effects).
    constexpr u64 KeepFrames = 120;
    const u64 tick = r.scheduler.CurrentTick();
    {
        const u64 done = r.scheduler.GetWorkSemaphore()->KnownGpuTick();
        while (!retired_views.empty() && retired_views.front().first <= done) {
            retired_views.pop_front();
        }
    }
    if (frame <= KeepFrames) {
        return;
    }
    trace.Next(1);
    // Read-only mirrors are reloadable copies, unlike GPU-written regions and images.
    // Reclaim a scene's cold mirrors even below the capacity budget; waiting until the
    // budget fills otherwise keeps every track's peak resident through later menus.
    constexpr u64 MirrorIdleFrames = 600;
    std::vector<std::pair<VAddr, u64>> cold_mirrors;
    if (frame % 30 == 0) {
        std::shared_lock map_lock{buffer_map_mutex};
        for (const auto& [base, mirror] : mirrors) {
            if (frame > mirror.used_frame && frame - mirror.used_frame > MirrorIdleFrames) {
                cold_mirrors.emplace_back(mirror.begin, mirror.end - mirror.begin);
            }
        }
    }
    // Mirror collection keeps its original cadence. Image retirement below is
    // incremental instead of concentrating 30 frames of retirement in one frame.
    for (const auto& [address, bytes] : cold_mirrors) {
        ++stats.mirrors_evicted;
        DropMirrors(address, bytes); // invalidates bindings; existing GPU-safe destruction
    }
    // FIX-157: a picture the GPU wrote stays past its idle time
    // while nothing else writes its memory: its pixels exist nowhere else (the GPU's writes stay
    // in the picture, not in guest memory). GT Sport bakes its light probes (two cube arrays of
    // 322 32x32 cubes, at 0x107aee1400 and 0x108626d400) with compute shaders while a race
    // loads; nothing read them for about three seconds around the start, they were dropped, the
    // race's textures there uploaded the zeros of guest memory, and the scene's ambient light
    // went: a dark, blue cockpit and world, with the odd warm frame. It is kept when guest memory
    // still holds what it held when the picture was made (nothing but the GPU wrote there:
    // keeping pictures whose memory the CPU had reused drew stale pictures, and writing them
    // back overwrote shader code the game had loaded there), no newer picture overlaps it, and
    // GPU work wrote no part of its memory since; from then on its pages are watched, and a CPU
    // write, an unmap or GPU work there lets it go. A baked image may be written in just one
    // frame and sampled much later; its short initial use does not make its GPU contents
    // disposable. Include room for the persistent 221 MB terrain shadow map alongside the
    // lighting probes; dropping that map makes its texture reload only the initial clear.
    // Kept images take at most 512 MB.
    constexpr u64 KeptBudget = 512ull << 20;
    u32 newly_kept = 0;
    const auto keep = [&](Native::ImageEntry& entry) {
        if (entry.from_cpu || !entry.image || entry.reused.load(std::memory_order_relaxed)) {
            return false;
        }
        const u64 version = entry.write_version.load(std::memory_order_relaxed);
        if (version == 0 || entry.foreign_clock.load(std::memory_order_relaxed) > version) {
            return false;
        }
        const VAddr address = entry.info.guest_address;
        const u64 size = std::max<u64>(entry.info.guest_size, 1);
        bool superseded = false;
        registry->ForEachOverlapping(address, address + size, [&](Native::ImageEntry& other) {
            superseded |= &other != &entry && !other.from_cpu &&
                          !other.reused.load(std::memory_order_relaxed) &&
                          other.write_version.load(std::memory_order_relaxed) > version;
        });
        if (superseded) {
            return false;
        }
        if (entry.kept.load(std::memory_order_relaxed)) {
            return true;
        }
        // A persistent depth map may live in GPU-only guest memory: there is no complete CPU
        // hash, and the region still holds its initial clear rather than the rendered depth.
        // Keep that authoritative image only when its backing has been watched since the first
        // upload and no CPU write or unmap occurred. The foreign-write and budget guards above
        // still apply; ordinary CPU-backed images continue to require an unchanged hash.
        const bool tracked_depth = entry.info.props.is_depth && entry.content_hash == 0 &&
                                   entry.guest_writes_tracked &&
                                   !entry.guest_modified.load(std::memory_order_seq_cst);
        if (kept_bytes + size > KeptBudget ||
            (!tracked_depth && !registry->MemoryUnchanged(entry))) {
            return false;
        }
        entry.kept.store(true, std::memory_order_seq_cst);
        if (tracked_depth && entry.guest_modified.load(std::memory_order_seq_cst)) {
            entry.kept.store(false, std::memory_order_relaxed);
            return false;
        }
        kept_bytes += size;
        WatchPages(address, size);
        ++newly_kept;
        if (Native::WatchedTexture(address, address + 1)) {
            LOG_WARNING(Render_Vulkan, "FIX-157 watch: {:#x} kept (version {}, frame {})",
                        address, version, frame);
        }
        return true;
    };
    trace.Next(2);
    registry->Evict(frame - KeepFrames, [&](Native::ImageEntry& entry) {
        retired_watch_ranges.emplace_back(
            VideoCore::PageManager::GetPageAddr(entry.info.guest_address),
            VideoCore::PageManager::GetNextPageAddr(
                entry.info.guest_address + std::max<u64>(entry.info.guest_size, 1) - 1));
        if (entry.kept.load(std::memory_order_relaxed)) {
            kept_bytes -= std::min<u64>(kept_bytes, std::max<u64>(entry.info.guest_size, 1));
        }
        if (Native::WatchedTexture(entry.info.guest_address, entry.info.guest_address + 1)) {
            LOG_WARNING(Render_Vulkan, "FIX-157 watch: {} {:#x} {}x{} layers {} evicted (last "
                        "use frame {}, version {}, frame {})",
                        entry.from_cpu ? "texture" : "target", entry.info.guest_address,
                        entry.info.size.width, entry.info.size.height,
                        entry.info.resources.layers, entry.last_use.load(),
                        entry.write_version.load(), frame);
        }
        {
            std::scoped_lock lock{views_mutex};
            if (const auto it = views.find(&entry); it != views.end()) {
                for (auto& record : it->second) {
                    retired_views.emplace_back(tick, std::move(record.view));
                }
                views.erase(it);
            }
        }
        held_versions.erase(&entry);
        std::erase(stream.written, &entry);
        std::erase(writebacks, &entry);
        std::erase(dump_entries, &entry);
        for (auto it = stencil_owners.begin(); it != stencil_owners.end();) {
            auto& owners = it->second;
            owners.erase(std::remove(owners.begin(), owners.end(), &entry), owners.end());
            it = owners.empty() ? stencil_owners.erase(it) : std::next(it);
        }
    }, keep, 16);
    if (newly_kept != 0) {
        static std::atomic<u32> logged{};
        if (const u32 n = logged++; n < 20 || n % 100 == 0) {
            LOG_WARNING(Render_Vulkan, "FIX-157: {} pictures the GPU wrote are kept past their idle "
                        "time ({:.1f} MB kept in all)", newly_kept, kept_bytes / 1048576.0);
        }
    }
}

void NativeRenderer::WatchPages(VAddr address, u64 size) {
    const VAddr begin = VideoCore::PageManager::GetPageAddr(address);
    const VAddr end = VideoCore::PageManager::GetNextPageAddr(address + size - 1);
    const auto fresh = page_watches.Track(begin,end,[&](VAddr run,u64 bytes) {
        r.page_manager.UpdatePageWatchers(run,bytes,VideoCore::PageOp::Track);
    });
    if (Native::WatchedTexture(begin,end)) {
        LOG_WARNING(Render_Vulkan, "FIX-153 watch: watch {:#x}-{:#x}, {} pages newly watched",
                    begin,end,fresh);
    }
}

void NativeRenderer::UnwatchPages(VAddr begin, VAddr end) {
    page_watches.Untrack(begin,end,[&](VAddr run,u64 bytes) {
        r.page_manager.UpdatePageWatchers(run,bytes,VideoCore::PageOp::Untrack);
    });
}

void NativeRenderer::ReleaseUnusedWatches() {
    Native::SlowCallTrace<2> trace{true, "retired-page-watches", {"live-ranges", "unwatch"}};
    if (retired_watch_ranges.empty()) {
        return;
    }
    // WatchPages coalesces every native owner into one page-manager watch. Removing one
    // resource may open only pages that no other image, region or mirror still needs.
    // In particular, retired mesh/texture memory can become a fiber stack: leaving its
    // unused pages read-only can prevent Windows from dispatching a subsequent write fault.
    std::vector<std::pair<VAddr, VAddr>> pending;
    pending.swap(retired_watch_ranges);
    std::ranges::sort(pending);
    std::vector<std::pair<VAddr, VAddr>> ranges;
    for (const auto& range : pending) {
        if (!ranges.empty() && range.first <= ranges.back().second) {
            ranges.back().second = std::max(ranges.back().second, range.second);
        } else {
            ranges.push_back(range);
        }
    }
    for (const auto& [begin, end] : ranges) {
        std::vector<std::pair<VAddr, VAddr>> live;
        const auto retain = [&](VAddr base, VAddr limit) {
            if (base < end && limit > begin) {
                live.emplace_back(std::max(begin, VideoCore::PageManager::GetPageAddr(base)),
                                  std::min(end, VideoCore::PageManager::GetNextPageAddr(limit - 1)));
            }
        };
        registry->ForEachOverlapping(begin, end, [&](Native::ImageEntry& entry) {
            retain(entry.info.guest_address,
                   entry.info.guest_address + std::max<u64>(entry.info.guest_size, 1));
        });
        for (const auto& [base, region] : regions) {
            retain(region.begin, region.end);
        }
        for (const auto& [base, mirror] : mirrors) {
            retain(mirror.begin, mirror.end);
        }
        std::ranges::sort(live);
        trace.Next(1, begin, end - begin);
        VAddr cursor = begin;
        for (const auto& [base, limit] : live) {
            if (base > cursor) {
                UnwatchPages(cursor, base);
            }
            cursor = std::max(cursor, limit);
        }
        if (cursor < end) {
            UnwatchPages(cursor, end);
        }
        trace.Next(0);
    }
}

void NativeRenderer::OnCpuWrite(VAddr address, u64 size, u64 exact) {
    // The written pages stop being watched (so they open up and the write goes on), and the
    // textures over them are uploaded again when next used (which watches them again).
    const VAddr begin = VideoCore::PageManager::GetPageAddr(address);
    const VAddr end = VideoCore::PageManager::GetNextPageAddr(address + std::max<u64>(size, 1) - 1);
    if (Native::WatchedTexture(begin, end)) {
        LOG_WARNING(Render_Vulkan, "FIX-153 watch: CPU write fault {:#x}+{:#x} (exact {:#x}); "
                    "memory {}", address, size, exact, Native::WatchedBytes(address));
    }
    // FIX-173: a region page holds what the GPU wrote there, which guest memory never got; a
    // CPU write to the page copies in only the bytes the CPU changed (RefreshRegion compares
    // with this copy of the page from before the write), not the whole page from guest memory.
    // Copying whole pages put stale guest bytes over the GPU's results: GT Sport computes the
    // cockpit's light from its interior capture into 64 bytes at 0x1000e317c0 every third
    // frame, and the CPU writes other data in that page every frame, so the cockpit was lit in
    // one frame of three and dark in the other two.
    if (end - begin <= 64_KB) {
        std::shared_lock map_lock{buffer_map_mutex};
        if (RegionOver(begin, end - begin)) {
            std::scoped_lock pending_lock{region_dirty_mutex};
            for (VAddr page = begin; page < end; page += 4096) {
                auto& slot = page_snapshots[page];
                if (slot) {
                    continue; // the first write since the page was copied in counts
                }
                auto copy = std::make_unique<std::array<u8, 4096>>();
                if (Core::RytyMemory::GuardedCopy(copy->data(),
                                                  reinterpret_cast<const void*>(page), 4096)) {
                    slot = std::move(copy);
                } else {
                    page_snapshots.erase(page);
                }
            }
        }
    }
    UnwatchPages(begin, end);
    {
        Native::UploadCause::ScopedCpuFault cause{address,size,exact};
        registry->CpuWrite(begin, end - begin, exact != 0 ? (address - begin) + exact : 0);
    }
    ++stats.cpu_writes;
    // NAT-114: pages of regions written by the CPU (a fault on any thread) are copied in before
    // the region's next use; the recorder sorts them to their regions.
    {
        std::scoped_lock pending_lock{region_dirty_mutex};
        pending_region_writes.emplace_back(begin, end);
    }
    has_pending_region_writes.store(true, std::memory_order_release);
}

u32 NativeRenderer::ReadGds(u32 gds_offset) {
    u32 value = 0;
    if (gds_offset + sizeof(u32) <= gds->Size()) {
        std::memcpy(&value, gds->Mapped() + gds_offset, sizeof(value));
    }
    return value;
}

void NativeRenderer::Splice() {
    std::scoped_lock lock{mutex};
    Stream& s = stream;
    if (!s.list) {
        return;
    }
    const u64 boundary_signal=ActiveSubmitSignal()?ActiveSubmitSignal():r.scheduler.CurrentTick();
    EndRendering();
    if (!writebacks.empty()) {
        GlobalBarrier(); // NAT-112: written back in this submission
    }
    const bool wrapped = s.wrapped_list != nullptr; // FIX-188: Intel Arc (#65)
    if (!wrapped) {
        s.list->End();
    }
    r.scheduler.EndRendering();
    if (auto* sidecar=FindParallelRuntime(this); sidecar) {
        const u64 signal=boundary_signal;
        ASSERT_MSG(sidecar->sync_pins.AssignPending(signal),"parallel synchronous pin epoch mismatch");
        sidecar->coordinator->Collect(r.scheduler.GetWorkSemaphore()->KnownGpuTick());
        sidecar->sync_pins.Collect(r.scheduler.GetWorkSemaphore()->KnownGpuTick(),
            [&](auto& callback) noexcept {return Native::Rhi::TryEnqueueCompleted(*device,callback);});
        if(sidecar->coordinator->PendingNodes()) {
        const auto parallel_wait_start=std::chrono::steady_clock::now();
        const bool drained=sidecar->coordinator->Drain(signal,
            [&](Native::Rhi::CommandList* list,Native::Parallel::PassCoordinator::JobPtr,
                Native::Parallel::PassCoordinator::PrefixPtr) noexcept {
                const vk::CommandBuffer cmd{reinterpret_cast<VkCommandBuffer>(list->NativeHandle())};
                r.scheduler.CommandBuffer().executeCommands(cmd);
                device->MarkExecuted(list,{timeline.get(),signal});
            });
        sidecar->drain_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-parallel_wait_start).count();
        ASSERT_MSG(drained,"parallel node preflight or submission epoch failed");
        }
    }
    if (!wrapped) {
        const vk::CommandBuffer secondary = s.cmd;
        r.scheduler.CommandBuffer().executeCommands(secondary);
    }
    const u64 tick = r.scheduler.CurrentTick();
    if (!wrapped) {
        device->MarkExecuted(s.list, {timeline.get(), tick});
    }
    {
        std::scoped_lock chunk_lock{chunk_mutex};
        RetireUploadChunks(s.chunks, retiring_chunks, s.chunk_used, tick, ChunkSize);
        for (const auto pool : s.pools) {
            retiring_pools.emplace_back(tick, pool);
        }
    }
    UploadMemoFor(this).Clear(); // bindings expire before upload chunks can be recycled
    for (auto& upload : s.flat_uploads) {
        upload.bytes.clear();
        upload.binding = {};
    }
    // The last partial standard chunk remains append-only across lists. The
    // immutable binding memo is still cleared above: only unused bytes survive.
    s.descriptor_reserve.Clear(); // never-written spares retire with their pools
    s.descriptor_sets.Clear(); // descriptor pools retire with this list
    if(auto* sidecar=FindParallelRuntime(this);sidecar && sidecar->ahead_context && sidecar->ahead_live) {
        const auto completed=r.scheduler.GetWorkSemaphore()->KnownGpuTick();
        for(auto& flight:sidecar->ahead_flights)if(flight.release) {
            if(!flight.tick) {ASSERT_MSG(flight.lease && flight.lease->lease->MarkUsed(boundary_signal),"ahead descriptor invalid signal");flight.tick=boundary_signal;}
            if(flight.tick<=completed) {flight.lease.reset();if(!Native::Rhi::TryEnqueueCompleted(*device,flight.release))continue;flight.tick=0;--sidecar->ahead_live;}
        }
    }
    s.pools.clear();
    s.keep.clear(); // freed by the device once the GPU ran this list (NAT-107)
    device->Collect(); // objects the GPU finished with (staging buffers of uploads)
    s.wrapped_list.reset();
    s.list = nullptr;
    s.cmd = vk::CommandBuffer{};
    s.rendering = false;
    if(auto* sidecar=FindParallelRuntime(this);sidecar && sidecar->transfer_mode_controller && !sidecar->closing) {
        const auto changed=sidecar->transfer_applied_mode.Apply(sidecar->transfer_mode_controller->Requested(),true);
        if(changed || !sidecar->transfer_mode_initial_logged) {
            sidecar->transfer_mode_initial_logged=true;
            const bool available=sidecar->coordinator->TransferEnabled();
#ifdef _WIN32
            LARGE_INTEGER qpc{},frequency{};SYSTEMTIME utc{};QueryPerformanceCounter(&qpc);QueryPerformanceFrequency(&frequency);GetSystemTime(&utc);
            LOG_INFO(Render_Vulkan,"PARALLEL-TRANSFER-MODE-APPLIED mode={} generation={} signal={} qpc={} frequency={} utc={:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z pool_available={}",
                sidecar->transfer_applied_mode.Enabled() && available,changed?changed->generation:0,boundary_signal,qpc.QuadPart,frequency.QuadPart,
                utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,utc.wMilliseconds,available);
#else
            LOG_INFO(Render_Vulkan,"PARALLEL-TRANSFER-MODE-APPLIED mode={} generation={} signal={} pool_available={}",sidecar->transfer_applied_mode.Enabled() && available,changed?changed->generation:0,boundary_signal,available);
#endif
        }
    }
    if(auto* sidecar=FindParallelRuntime(this);sidecar && sidecar->view_mode_controller && !sidecar->closing) {
        const auto changed=sidecar->view_applied_mode.Apply(sidecar->view_mode_controller->Requested(),true);
        if(changed || !sidecar->view_mode_initial_logged) {
            sidecar->view_mode_initial_logged=true;
#ifdef _WIN32
            LARGE_INTEGER qpc{},frequency{};SYSTEMTIME utc{};
            QueryPerformanceCounter(&qpc);QueryPerformanceFrequency(&frequency);GetSystemTime(&utc);
            LOG_INFO(Render_Vulkan,"PARALLEL-VIEW-MODE-APPLIED mode={} generation={} signal={} qpc={} frequency={} utc={:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
                sidecar->view_applied_mode.Enabled(),changed?changed->generation:0,boundary_signal,qpc.QuadPart,frequency.QuadPart,
                utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,utc.wMilliseconds);
#else
            LOG_INFO(Render_Vulkan,"PARALLEL-VIEW-MODE-APPLIED mode={} generation={} signal={}",sidecar->view_applied_mode.Enabled(),changed?changed->generation:0,boundary_signal);
#endif
        }
    }
    // A mode change takes effect only after this stream and all ordered nodes
    // have been appended and owner pins have their submission signal.
    if(auto* sidecar=FindParallelRuntime(this);sidecar && sidecar->mode_controller && !sidecar->closing) {
        const u64 signal=boundary_signal;
        const auto request=sidecar->mode_controller->Requested();
        if(sidecar->ahead_context && !request.enabled) {
            std::scoped_lock ahead_lock{ahead_context_mutex};
            SetAheadMode(*sidecar->ahead_context,false);
        }
        if(sidecar->prepared_proof_context && !request.enabled)sidecar->prepared_proof_context->SetMode(false);
        const auto changed=sidecar->applied_mode.Apply(request,true);
        if(sidecar->ahead_context && sidecar->applied_mode.Enabled()) {
            std::scoped_lock ahead_lock{ahead_context_mutex};
            if(!sidecar->ahead_context->closing.load(std::memory_order_acquire))
                SetAheadMode(*sidecar->ahead_context,true);
        }
        if(sidecar->prepared_proof_context && sidecar->applied_mode.Enabled())sidecar->prepared_proof_context->SetMode(true);
        if((changed || !sidecar->mode_initial_logged) && sidecar->prepared_proof_context) {
            const auto state=sidecar->prepared_proof_context->state.load(std::memory_order_seq_cst);
            const auto active=sidecar->prepared_proof_context->active.load(std::memory_order_seq_cst);
#ifdef _WIN32
            LARGE_INTEGER qpc{},frequency{};SYSTEMTIME utc{};QueryPerformanceCounter(&qpc);QueryPerformanceFrequency(&frequency);GetSystemTime(&utc);
            LOG_INFO(Render_Vulkan,"PARALLEL-PREPARED-VIEW-STATE mode={} generation={} active={} control_generation={} signal={} qpc={} frequency={} utc={:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
                bool(state&1),state>>1,active,changed?changed->generation:0,signal,qpc.QuadPart,frequency.QuadPart,
                utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,utc.wMilliseconds);
#else
            LOG_INFO(Render_Vulkan,"PARALLEL-PREPARED-VIEW-STATE mode={} generation={} active={} control_generation={} signal={}",bool(state&1),state>>1,active,changed?changed->generation:0,signal);
#endif
        }
        if((changed || !sidecar->mode_initial_logged) && sidecar->ahead_context) {
            const auto& a=*sidecar->ahead_context;
            LOG_INFO(Render_Vulkan,"PARALLEL-PRODUCER-MODE mode={} preparing={} pool_quiescent={}",
                a.mode.load(std::memory_order_acquire),a.active.load(std::memory_order_acquire),a.pools.Quiescent());
        }
        if(changed || !sidecar->mode_initial_logged) {
            sidecar->mode_initial_logged=true;
#ifdef _WIN32
            LARGE_INTEGER qpc{},frequency{};SYSTEMTIME utc{};
            QueryPerformanceCounter(&qpc);QueryPerformanceFrequency(&frequency);GetSystemTime(&utc);
            LOG_INFO(Render_Vulkan,"PARALLEL-MODE-APPLIED mode={} generation={} signal={} qpc={} frequency={} utc={:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
                sidecar->applied_mode.Enabled(),changed?changed->generation:0,signal,qpc.QuadPart,frequency.QuadPart,
                utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,utc.wMilliseconds);
#else
            LOG_INFO(Render_Vulkan,"PARALLEL-MODE-APPLIED mode={} generation={} signal={}",sidecar->applied_mode.Enabled(),changed?changed->generation:0,signal);
#endif
        }
    }
    ++stats.splices;
}

void NativeRenderer::CopyDisplayCrop(const Native::ImageEntry& source, Native::ImageEntry& crop,
                                     vk::Extent2D visible) {
    registry->QueueUploadForUse(*const_cast<Native::ImageEntry*>(&source));
    Stream& s = Begin();
    PrepareImages();
    GlobalBarrier();
    const vk::ImageCopy region{
        .srcSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .dstSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .extent{visible.width, visible.height, 1},
    };
    CmdBuf{s.cmd}.copyImage(vk::Image{Handle<VkImage_T>(source.image->NativeHandle())},
                            vk::ImageLayout::eGeneral,
                            vk::Image{Handle<VkImage_T>(crop.image->NativeHandle())},
                            vk::ImageLayout::eGeneral, region);
    crop.rewrite = Native::ImageEntry::Rewrite::None;
    Written(&crop);
    s.read.push_back(&source);
    s.written.push_back(&crop);
    s.buffers_written = true;
    ++s.commands;
}

const Native::ImageEntry* NativeRenderer::CropDisplay(const Native::ImageEntry& entry,
                                                      vk::Extent2D visible) {
    const auto& info = entry.info;
    if (info.size.width == visible.width && info.size.height == visible.height) {
        return &entry;
    }
    if (visible.width == 0 || visible.height == 0 || info.size.width < visible.width ||
        info.size.height < visible.height || info.num_samples != 1 || info.props.is_depth ||
        info.props.is_block || info.type != AmdGpu::ImageType::Color2D ||
        info.resources.levels != 1 || info.resources.layers != 1) {
        return nullptr;
    }
    auto crop_info = info;
    crop_info.size.width = visible.width;
    crop_info.size.height = visible.height;
    crop_info.UpdateSize();
    auto* crop = registry->Find(crop_info, false, frame);
    if (!crop || !crop->image || crop->info.size.width != visible.width ||
        crop->info.size.height != visible.height) {
        return nullptr;
    }
    CopyDisplayCrop(entry, *crop, visible);
    static std::atomic<u32> logged{};
    if (Sample(logged)) {
        LOG_INFO(Render_Vulkan,
                 "Native display: cropped {}x{} from {}x{} backing at {:#x}, pitch {}",
                 visible.width, visible.height, info.size.width, info.size.height,
                 info.guest_address, info.pitch);
    }
    return crop;
}

std::optional<NativeRenderer::Display> NativeRenderer::FindDisplay(const VideoCore::ImageInfo& info,
                                                                   vk::Format view_format,
                                                                   vk::Extent2D output_size) {
    std::scoped_lock lock{mutex};
    (void)A82CaptureAfterRace(); // Observe race even when target shader does not run.
    if (InPlaceSampled()) {
        for (u32 i = 0; i < in_place.pages.size(); ++i) {
            in_place.total[i] += in_place.pages[i].TakePageCount();
        }
        ++in_place.frames;
    }
    ++frame;
    shared_frame.store(frame, std::memory_order_relaxed); // OPT-010
    // The game's render target there (its layout from the CB registers may differ in details
    // from the video out description), else an image of the display layout.
    Native::ImageEntry* entry =
        registry->FindExisting(info.guest_address, [&](const Native::ImageEntry& candidate) {
            return !candidate.from_cpu && !candidate.info.props.is_depth &&
                   candidate.info.size.width >= info.size.width &&
                   candidate.info.size.height >= info.size.height &&
                   candidate.info.num_bits == info.num_bits && candidate.info.pitch == info.pitch &&
                   candidate.info.num_samples == 1 && candidate.info.resources.levels == 1 &&
                   candidate.info.resources.layers == 1;
        });
    if (!entry) {
        static std::atomic<u32> logged{};
        if (Sample(logged)) {
            LOG_WARNING(Render_Vulkan,
                        "NAT-103: nothing drew the display buffer {:#x} ({}x{}, {} bits) yet",
                        info.guest_address, info.size.width, info.size.height, info.num_bits);
        }
        entry = registry->Find(info, true, frame);
    }
    if (!entry || !entry->image) {
        return std::nullopt;
    }
    const auto* source = CropDisplay(*entry, {info.size.width, info.size.height});
    if (!source) {
        return std::nullopt;
    }
    VideoCore::ImageViewInfo view_info{};
    view_info.format = view_format;
    view_info.mapping.a = AmdGpu::CompSwizzle::One; // as the presenter: no alpha blending
    const vk::ImageView view = View(*source, view_info, ViewUse::Sampled);
    if (!view) {
        return std::nullopt;
    }
    // A display buffer nothing drew into yet starts in the general layout too.
    if (device->PendingImageInit()) {
        Begin();
        PrepareImages();
    }
    ProbeDisplay(*source); // FIX-175 (diagnosis)
    Splice();
    Display display{vk::Image{Handle<VkImage_T>(source->image->NativeHandle())}, view,
                    source->info.size.width, source->info.size.height};
    if (const auto shown =
            UiHires::TakeFrame(r.instance, r.scheduler.DirectCommandBuffer(), info.guest_address,
                               {display.width, display.height}, output_size, view_format)) {
        display.output_view = shown->view;
        display.ui_layer = shown->ui;
        display.output_size = shown->size;
        display.dlss_scaled = shown->dlss;
    }
    if (!fsr_display_backend.empty()) {
        static u32 fsr_present_logged{};
        if (fsr_present_logged++ < 16)
            LOG_INFO(Render_Vulkan, "NATIVE-FSR: present clock {} reconstruction clock {} buffer {:#x} reconstructed buffer {:#x}",
                frame, fsr_display_frame, entry->info.guest_address, fsr_display_address);
    }
    if (fsr_display_frame + 1 == frame && fsr_display_address == entry->info.guest_address)
        display.upscaler = fsr_display_backend;
    VerifyMirrors(); // FIX-176 (diagnosis)
    EvictImages(); // (the display picture was just used: it stays)
    ReleaseUnusedWatches();
    return display;
}

u64 NativeRenderer::MirrorVerifyBudget() {
    static const u64 budget = [] {
        const char* env = std::getenv("RYGT_NATIVE_MIRROR_VERIFY");
        return env && *env ? std::strtoull(env, nullptr, 10) * 1_MB : u64{0};
    }();
    return budget;
}

void NativeRenderer::VerifyMirrors() {
    // FIX-176 (diagnosis, RYGT_NATIVE_MIRROR_VERIFY=<MB a frame>): guest memory under the mirrors,
    // a few MB a frame, hashed per 64 KB granule and compared with the hash taken the last time;
    // a change no CPU write fault or copy-in accounts for means the mirror went stale unseen (the
    // tunnel explosions went away with RYGT_NATIVE_MIRRORS=0). Such granules are logged and
    // copied in again.
    const u64 budget = MirrorVerifyBudget();
    if (budget == 0 || mirrors.empty()) {
        return;
    }
    SortRegionWrites();
    static thread_local std::vector<u8> scratch(MirrorGranule);
    u64 hashed = 0;
    u32 changed = 0;
    auto it = mirrors.lower_bound(mirror_verify_cursor);
    for (u32 visited = 0; hashed < budget && visited < 2 * mirrors.size() + 2; ++visited) {
        if (it == mirrors.end()) {
            it = mirrors.begin();
            mirror_verify_cursor = 0;
            if (it == mirrors.end()) {
                break;
            }
        }
        Mirror& mirror = it->second;
        const u64 granules = (mirror.end - mirror.begin + MirrorGranule - 1) / MirrorGranule;
        if (mirror.hashes.size() != granules) {
            mirror.hashes.assign(granules, 0);
        }
        u64 g = mirror_verify_cursor > mirror.begin && mirror_verify_cursor < mirror.end
                    ? (mirror_verify_cursor - mirror.begin) / MirrorGranule
                    : 0;
        for (; g < granules && hashed < budget; ++g) {
            const VAddr from = mirror.begin + g * MirrorGranule;
            const u64 size = std::min<u64>(MirrorGranule, mirror.end - from);
            const bool dirty = std::ranges::any_of(mirror.dirty_pages, [&](VAddr page) {
                return page >= from && page < from + size;
            });
            if (dirty) {
                mirror.hashes[g] = 0; // copied in at the next use, hashed again after
                continue;
            }
            if (!Core::RytyMemory::GuardedCopy(scratch.data(), reinterpret_cast<const void*>(from),
                                               size)) {
                continue;
            }
            const u64 hash = XXH3_64bits(scratch.data(), size) | 1;
            hashed += size;
            if (mirror.hashes[g] != 0 && mirror.hashes[g] != hash) {
                // A CPU write that faulted just now is no news: it waits in the pending writes
                // (not sorted here: sorting can drop this very mirror).
                bool pending = false;
                {
                    std::scoped_lock lock{region_dirty_mutex};
                    pending = std::ranges::any_of(pending_region_writes, [&](const auto& w) {
                        return w.first < from + size && w.second > from;
                    });
                }
                if (pending) {
                    mirror.hashes[g] = 0;
                    continue;
                }
                ++changed;
                ++stats.mirror_untrapped;
                static std::atomic<u32> logged{};
                if (Sample(logged)) {
                    LOG_WARNING(Render_Vulkan,
                                "FIX-176: mirror {:#x}+{:#x} granule {:#x}+{:#x} changed with no "
                                "CPU write seen (frame {}, mirror last used frame {})",
                                mirror.begin, mirror.end - mirror.begin, from, size, frame,
                                mirror.used_frame);
                }
                for (VAddr page = from; page < from + size; page += 4096) {
                    mirror.dirty_pages.push_back(page); // copied in again at the next use
                }
                mirror.hashes[g] = 0;
                continue;
            }
            mirror.hashes[g] = hash;
        }
        if (g >= granules) {
            ++it;
            mirror_verify_cursor = it == mirrors.end() ? 0 : it->first;
        } else {
            mirror_verify_cursor = mirror.begin + g * MirrorGranule;
        }
    }
    if (changed != 0) {
        ++buffer_generation; // OPT-003: memos see the dirty pages
    }
}

void NativeRenderer::ProbeDisplay(const Native::ImageEntry& entry) {
    // FIX-175 (diagnosis, RYGT_NATIVE_WATCH_DISPLAY=x,y,w,h): a rectangle of each picture the
    // presenter shows, read back from the GPU: the mean brightness of its top, middle and bottom
    // thirds, one line per presented frame (single-frame glitches stand out against their
    // neighbours; the game's working targets are no good for that, the next frame's work may
    // already have cleared them when a probe runs).
    static const std::array<u32, 4> rect = [] {
        std::array<u32, 4> r{};
        if (const char* env = std::getenv("RYGT_NATIVE_WATCH_DISPLAY"); env && *env) {
            std::sscanf(env, "%u,%u,%u,%u", &r[0], &r[1], &r[2], &r[3]);
        }
        return r;
    }();
    if (rect[2] == 0 || rect[3] < 3 || !entry.image) {
        return;
    }
    const u64 done = r.scheduler.GetWorkSemaphore()->KnownGpuTick();
    for (auto it = display_probes.begin(); it != display_probes.end();) {
        if (it->tick > done) {
            ++it;
            continue;
        }
        const u8* bytes = it->buffer->Mapped();
        const u32 w = rect[2], third = rect[3] / 3;
        std::array<double, 3> part{};
        for (u32 band = 0; band < 3; ++band) {
            for (u64 y = u64(band) * third; y < u64(band + 1) * third; ++y) {
                for (u64 x = 0; x < w; ++x) {
                    u32 word;
                    std::memcpy(&word, bytes + (y * w + x) * 4, 4);
                    part[band] += double((word & 1023) + ((word >> 10) & 1023) +
                                         ((word >> 20) & 1023)) / (3.0 * 1023.0);
                }
            }
            part[band] /= double(third) * w;
        }
        LOG_WARNING(Render_Vulkan, "FIX-175 display: frame {} {:#x} thirds: {:.4f} {:.4f} {:.4f}",
                    it->frame, it->address, part[0], part[1], part[2]);
        // FIX-182 (diagnosis): a frame much darker than the one before in two bands or more is
        // saved with that one (raw A2B10G10R10, user\log\glitch_<frame>_<now|before>.raw).
        u32 darker = 0;
        for (u32 band = 0; band < 3; ++band) {
            darker += display_last_part[band] > 0.03 && part[band] < 0.45 * display_last_part[band];
        }
        if (darker >= 2 && display_glitch_dumps < 12 && !display_last_bytes.empty()) {
            ++display_glitch_dumps;
            const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
            const auto save = [&](const char* name, const u8* data) {
                const auto path = dir / fmt::format("glitch_{}_{}_{}x{}.raw", it->frame, name, w,
                                                    rect[3]);
                if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
                    std::fwrite(data, 1, u64(w) * rect[3] * 4, file);
                    std::fclose(file);
                }
            };
            save("now", bytes);
            save("before", display_last_bytes.data());
            LOG_WARNING(Render_Vulkan, "FIX-182: frame {} saved (darker than the one before)",
                        it->frame);
        }
        display_last_part = part;
        display_last_bytes.assign(bytes, bytes + u64(w) * rect[3] * 4);
        it = display_probes.erase(it);
    }
    const auto& desc = entry.image->Desc();
    if (display_probes.size() >= 16 || rect[0] + rect[2] > desc.width ||
        rect[1] + rect[3] > desc.height || entry.info.num_bits != 32) {
        return;
    }
    auto buffer = device->CreateBuffer({.size = u64(rect[2]) * rect[3] * 4,
                                        .usage = Native::Rhi::BufferUsageStorage |
                                                 Native::Rhi::BufferUsageTransferDst,
                                        .memory = Native::Rhi::MemoryKind::Readback,
                                        .name = "display probe"});
    if (!buffer || !buffer->Mapped()) {
        return;
    }
    Begin();
    GlobalBarrier();
    stream.cmd.copyImageToBuffer(
        vk::Image{Handle<VkImage_T>(entry.image->NativeHandle())}, vk::ImageLayout::eGeneral,
        vk::Buffer{Handle<VkBuffer_T>(buffer->NativeHandle())},
        vk::BufferImageCopy{.bufferOffset = 0,
                            .imageSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor,
                                              .mipLevel = 0,
                                              .baseArrayLayer = 0,
                                              .layerCount = 1},
                            .imageOffset = {s32(rect[0]), s32(rect[1]), 0},
                            .imageExtent = {rect[2], rect[3], 1}});
    display_probes.push_back({std::move(buffer), r.scheduler.CurrentTick() + 1,
                              u64(rect[2]) * rect[3] * 4, frame, 0, rect[2], rect[3], true,
                              entry.info.guest_address});
}

void NativeRenderer::DumpFrame(const std::filesystem::path& dir) {
    std::scoped_lock lock{mutex};
    struct Readback {
        const Native::ImageEntry* entry;
        std::unique_ptr<Native::Rhi::Buffer> buffer;
        u64 size;
        u32 index;
        u32 mip;
    };
    std::vector<Readback> readbacks;
    Stream& s = Begin();
    GlobalBarrier();
    // NAT-123: the sampled GPU-written textures follow the targets (named tex_<key>).
    std::vector<std::pair<const Native::ImageEntry*, std::string>> jobs;
    {
        u32 order = 0;
        for (const auto* entry : dump_entries) {
            jobs.emplace_back(entry, fmt::format("{:03}_{:x}_{}x{}_{}{}.bin", order++,
                                                 entry->info.guest_address,
                                                 entry->image ? entry->image->Desc().width : 0,
                                                 entry->image ? entry->image->Desc().height : 0,
                                                 entry->image ? u32(entry->image->Desc().format)
                                                              : 0,
                                                 entry->info.props.is_depth ? "_depth" : ""));
        }
        std::string list;
        for (const auto& [key, entry] : dump_textures) {
            u32 sources;
            {
                std::scoped_lock lock{const_cast<Native::ImageEntry*>(entry)->sources_mutex};
                sources = u32(entry->sources.size());
            }
            const auto& info = entry->info;
            // FIX-153 (diagnosis): what guest memory holds there, and whether the GPU writes it
            // in a region.
            std::array<u32, 4> head{};
            const bool readable = Core::RytyMemory::GuardedCopy(
                head.data(), reinterpret_cast<const void*>(info.guest_address), sizeof(head));
            const Region* region =
                RegionOver(info.guest_address, std::max<u64>(info.guest_size, 1));
            // The bytes there now against the bytes last uploaded (NAT-109's hash).
            u64 hash_now = 0;
            const u64 size = info.guest_size;
            if (size != 0 && size <= (4ull << 20) && r.memory->IsValidMapping(info.guest_address) &&
                r.memory->ClampRangeSize(info.guest_address, size) == size &&
                Native::ReadablePrefix(reinterpret_cast<const void*>(info.guest_address), size) ==
                    size) {
                hash_now = Core::HashGuestBytes(info.guest_address, size) | 1;
            }
            list += fmt::format("{} -> {} {:#x} {}x{} mips {} layers {} {}, {} sources, "
                                "version {}{}; memory {}; {}; uploads {}, stale {}; "
                                "total uploads {} kind {}, tiled {} mode {} bits {} size {:#x}; "
                                "hash now {:016x} uploaded {:016x}{}\n",
                                key, entry->from_cpu ? "texture" : "target", info.guest_address,
                                info.size.width, info.size.height, info.resources.levels,
                                info.resources.layers, vk::to_string(info.pixel_format), sources,
                                entry->write_version.load(),
                                info.num_samples > 1 ? " (multisampled, not dumped)" : "",
                                readable ? fmt::format("{:08x} {:08x} {:08x} {:08x}", head[0],
                                                       head[1], head[2], head[3])
                                         : std::string{"unreadable"},
                                region ? fmt::format("in region {:#x}+{:#x}", region->begin,
                                                     region->end - region->begin)
                                       : std::string{"no region"},
                                entry->upload_count.load(), entry->needs_upload.load(),
                                entry->total_uploads.load(), entry->upload_kind,
                                info.props.is_tiled, u32(info.tile_mode), info.num_bits, size,
                                hash_now, entry->content_hash,
                                hash_now && entry->content_hash && hash_now != entry->content_hash
                                    ? " CHANGED"
                                    : "");
            jobs.emplace_back(entry, fmt::format("tex_{}{}.bin", key,
                                                 info.props.is_depth ? "_depth" : ""));
        }
        if (std::FILE* file = std::fopen((dir / "textures.txt").string().c_str(), "w")) {
            std::fwrite(list.data(), 1, list.size(), file);
            std::fclose(file);
        }
    }
    u64 extra_bytes=0, total_readback_bytes=0;
    constexpr u64 TraceReadbackBudget=128ull<<20;
    std::string mip_manifest="file,address,mip,width,height,depth,layers,unit_bytes,compressed,bytes,status,version,format,depth_aspect\n";
    u32 index=0;
    for(const auto& [entry,name]:jobs){
        const u32 order=index++;
        if(!entry->image)continue;
        const auto& desc=entry->image->Desc();
        const bool block=entry->info.props.is_block;
        const u32 unit=entry->info.props.is_depth?(u32(desc.format)==u32(vk::Format::eD16Unorm)?2u:4u):std::max(entry->info.num_bits/8,1u);
        const u32 layers=desc.dim==Native::Rhi::ImageDim::D3?1u:std::max(desc.layers,1u);
        for(u32 mip=0;mip<std::min(desc.mips,32u);++mip){
            const auto l=SceneTrace::Layout(desc.width,desc.height,desc.dim==Native::Rhi::ImageDim::D3?desc.depth:1u,layers,mip,unit,block);
            const std::string file=mip==0?name:std::filesystem::path(name).stem().string()+fmt::format("_mip{}.bin",mip);
            const char* status="queued";
            if(desc.samples!=1)status="skipped_msaa";
            else if(!(desc.usage & Native::Rhi::ImageUsageTransferSrc))status="skipped_no_transfer_src";
            else if(!l.bytes)status="skipped_layout";
            else if(mip==0 && entry->from_cpu)status="metadata_only_cpu_base";
            else if(mip>0 && l.bytes>(64ull<<20)-extra_bytes)status="skipped_extra_mip_budget";
            else if(l.bytes>TraceReadbackBudget-total_readback_bytes)status="skipped_total_readback_budget";
            if(std::string_view(status)!="queued"){
                mip_manifest+=fmt::format("{},{:#x},{},{},{},{},{},{},{},{},{},{},{},{}\n",file,entry->info.guest_address,mip,l.width,l.height,l.depth,l.layers,unit,block,l.bytes,status,entry->write_version.load(),u32(desc.format),bool(entry->info.props.is_depth));continue;
            }
            auto buffer=device->CreateBuffer({.size=l.bytes,.usage=Native::Rhi::BufferUsageTransferDst,.memory=Native::Rhi::MemoryKind::Readback,.name="scene trace mip"});
            if(!buffer || !buffer->Mapped())status="allocation_failed";
            else {
                if(mip>0)extra_bytes+=l.bytes;
                total_readback_bytes+=l.bytes;
                s.cmd.copyImageToBuffer(vk::Image{Handle<VkImage_T>(entry->image->NativeHandle())},vk::ImageLayout::eGeneral,
                    vk::Buffer{Handle<VkBuffer_T>(buffer->NativeHandle())},vk::BufferImageCopy{.bufferOffset=0,.bufferRowLength=l.row,.bufferImageHeight=l.rows,
                    .imageSubresource{.aspectMask=entry->info.props.is_depth?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor,.mipLevel=mip,.baseArrayLayer=0,.layerCount=l.layers},
                    .imageOffset={0,0,0},.imageExtent={l.width,l.height,l.depth}});
                readbacks.push_back({entry,std::move(buffer),l.bytes,order,mip});
            }
            mip_manifest+=fmt::format("{},{:#x},{},{},{},{},{},{},{},{},{},{},{},{}\n",file,entry->info.guest_address,mip,l.width,l.height,l.depth,l.layers,unit,block,l.bytes,status,entry->write_version.load(),u32(desc.format),bool(entry->info.props.is_depth));
        }
    }
    if(auto* f=std::fopen((dir/"mip-manifest.csv").string().c_str(),"w")){std::fwrite(mip_manifest.data(),1,mip_manifest.size(),f);std::fclose(f);}
    const vk::MemoryBarrier2 trace_to_host{.srcStageMask=vk::PipelineStageFlagBits2::eAllTransfer,.srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
    s.cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount=1,.pMemoryBarriers=&trace_to_host});
    Splice();
    r.scheduler.Finish();
    if (!g_pixel_trace.hashes.empty()) {
        std::ofstream out{dir / "pixel-trace.txt"};
        const auto* words = reinterpret_cast<const u32*>(g_pixel_trace.buffer->Mapped());
        for (size_t i = 0; i < g_pixel_trace.hashes.size(); ++i) {
            out << fmt::format("{} vs {:#x} ps {:#x}", i, g_pixel_trace.hashes[i].first,
                               g_pixel_trace.hashes[i].second);
            for (size_t p = 0; p < TracedPixels.size(); ++p) {
                out << fmt::format(" {:08x}", words[i * TracedPixels.size() + p]);
            }
            out << "\n";
        }
        g_pixel_trace.hashes.clear();
    }
    for (const auto& readback : readbacks) {
        const auto& info = readback.entry->info;
        const auto& desc = readback.entry->image->Desc();
        const auto& name=jobs[readback.index].second;
        const auto path=dir/(readback.mip==0?name:std::filesystem::path(name).stem().string()+fmt::format("_mip{}.bin",readback.mip));
        readback.buffer->Invalidate();
        if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
            std::fwrite(readback.buffer->Mapped(), 1, readback.size, file);
            std::fclose(file);
        }
    }
    LOG_WARNING(Render_Vulkan, "NAT-116: {} render targets and textures of the frame dumped to {}",
                readbacks.size(), dir.string());
    if(scene_indirect_buffer){
        scene_indirect_buffer->Invalidate();
        if(auto* f=std::fopen((dir/"indirect-arguments.csv").string().c_str(),"w")){
            std::fputs("id,guest_address,guest_offset,x,y,z\n",f);
            for(size_t i=0;i<scene_indirect_args.size();++i){const auto& arg=scene_indirect_args[i];std::array<u32,3> xyz{};std::memcpy(xyz.data(),scene_indirect_buffer->Mapped()+i*16,12);std::fprintf(f,"%llu,0x%llx,%llu,%u,%u,%u\n",arg.id,arg.address,arg.offset,xyz[0],xyz[1],xyz[2]);}std::fclose(f);
        }
    }
    scene_indirect_buffer.reset();scene_indirect_args.clear();
    SceneTrace::Save(dir,r.instance.GetDevice());
    dump_entries.clear();
    dump_textures.clear();
}

void NativeRenderer::Report() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report < std::chrono::seconds{10}) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - last_report).count();
    last_report = now;
    const auto& registry_stats = registry->GetStats();
    const u64 uploads = registry_stats.uploads.load();
    const u64 upload_bytes = registry_stats.upload_bytes.load();
    const u64 evicted = registry_stats.evicted.load();
    std::array<u64, 3> stale{};
    for (u32 i = 0; i < stale.size(); ++i) {
        stale[i] = registry_stats.stale[i].load();
    }
    // The textures uploaded most in the window.
    std::vector<std::tuple<u32, VAddr, u32, u32, u64, bool>> busy;
    registry->ForEachOverlapping(0, ~VAddr{0}, [&](Native::ImageEntry& entry) {
        if (const u32 count = entry.upload_count.exchange(0); count >= 4) {
            busy.emplace_back(count, entry.info.guest_address, entry.info.size.width,
                              entry.info.size.height, entry.info.guest_size, entry.from_cpu);
        }
    });
    std::ranges::sort(busy, std::greater{});
    std::string busiest;
    for (size_t i = 0; i < std::min<size_t>(busy.size(), 5); ++i) {
        const auto& [count, address, width, height, size, texture] = busy[i];
        busiest += fmt::format(" {} {:#x} {}x{} ({} KB) x{};", texture ? "texture" : "target",
                               address, width, height, size / 1024, count);
    }
    std::string crowded;
    for (const auto& [address, count] : registry->Crowded(4)) {
        crowded += fmt::format(" {:#x} x{};", address, count);
    }
    LOG_INFO(Render_Vulkan,
             "NAT-108 images (10 s): {} uploads ({:.1f} MB), {} skipped as unchanged (NAT-109), "
             "textures made stale by CPU writes {}, unmaps {}, GPU writes {} ({} targets so far, "
             "NAT-119); {} evicted, {} live, most at one address:{}; uploaded most:{}",
             uploads - report_base.uploads, (upload_bytes - report_base.upload_bytes) / 1048576.0,
             registry->GetUploadStats().unchanged.exchange(0),
             stale[0] - report_base.stale[0], stale[1] - report_base.stale[1],
             stale[2] - report_base.stale[2], registry_stats.targets_rewritten.load(),
             evicted - report_base.evicted, registry->Count(),
             crowded,
             busiest.empty() ? std::string{" none"} : busiest);
    report_base = {uploads, upload_bytes, evicted, stale};
    LOG_INFO(Render_Vulkan,
             "NAT-100 native (10 s): {} draws, {} dispatches, {} skipped, {} without pipeline; "
             "{} passes, {} barriers ({} pass barriers, {} passes unblocked, NAT-140), {} lists "
             "run; {} reads copied ({:.1f} MB), {} ranges bound "
             "empty, {} images bound empty; {} fills, {} copies, {} meta clears, {} inline "
             "writes, {} pictures copied from targets, {} written back, {} CPU writes to textures, "
             "{} ranges gathered, region binds/made/refreshes {}/{}/{} (NAT-114; {} regions, "
             "{:.1f} MB, {} pages refreshed in {:.1f} ms; {} guest buffers in {:.1f} ms), "
             "mirrors {} ({:.1f} MB; NAT-150: {} binds, {} made ({:.1f} MB), {} dropped as "
             "CPU-written, {} evicted, {} over budget, {} pages refreshed, {} granules read in "
             "place), "
             "memo hits textures {}/{} buffers {} (OPT-002/003), vertex ranges in the "
             "previous one's mirror {} (OPT-004), vertex records skipped {} (OPT-005), "
             "prepared draws (OPT-010): {} used, {} without one, rejected (pipeline/user "
             "data/unmap/images/buffers) {}, made on job threads {} ({:.2f} us each, {} could "
             "not, {} replaced, {} stale), vertices from them {} ({} not), pipelines from "
             "them {} (OPT-012), record parts "
             "barrier/begin/dynamic/vertex/pipeline/bindings/draw/notes {}, "
             "fill shaders {}, targets rewritten by clears/uploads/partial clears/fills "
             "{}/{}/{}/{} (NAT-120/124/132), "
             "on the GPU ({:.1f} MB), uploads from guest memory on the GPU/CPU {}/{}; images so far: "
             "{} lookups, {} textures and {} targets made, {} uploads ({:.1f} MB); {:.0f} draws/s, "
             "{:.2f} us a draw, {:.2f} us a dispatch on the recorder; images bound empty by "
             "reason (none/unmapped/format/geometry/outside/FIX-081/no image) {}; us per draw in "
             "pipeline/targets/buffers/textures(sources)/vertices/prepare/record {}",
             stats.draws, stats.dispatches, stats.skipped, stats.no_pipeline, stats.passes,
             stats.barriers, stats.pass_barriers, stats.passes_unblocked, stats.splices,
             stats.copied_reads, stats.copied_bytes / 1048576.0,
             stats.unbound, stats.null_images, stats.fills, stats.copies, stats.meta_clears,
             stats.inline_writes, stats.source_copies, stats.writebacks,
             stats.cpu_writes.exchange(0),
             stats.gathers, stats.region_binds, stats.regions_made, stats.region_refreshes,
             regions.size(),
             [&] {
                 u64 bytes = 0;
                 for (const auto& [base, region] : regions) {
                     bytes += region.end - region.begin;
                 }
                 return bytes / 1048576.0;
             }(),
             stats.region_refresh_pages, stats.region_refresh_ns / 1e6, stats.guest_buffer_calls,
             stats.guest_buffer_ns * 16 / 1e6, // OPT-001: timed on one draw in 16
             mirrors.size(), mirror_bytes / 1048576.0,
             stats.mirror_binds, stats.mirrors_made, stats.mirror_made_bytes / 1048576.0,
             stats.mirrors_dropped, stats.mirrors_evicted, stats.mirror_over_budget,
             stats.mirror_refresh_pages, dynamic_granules.size(), stats.texture_memo_hits,
             stats.texture_lookups, stats.buffer_memo_hits, stats.vertex_near,
             stats.vertex_skipped, stats.prepared_used, stats.prepared_missing,
             fmt::join(stats.prepared_rejected, "/"), prepare_stats.made.load(),
             prepare_stats.made.load() ? prepare_stats.ns.load() / 1000.0 / prepare_stats.made.load()
                                       : 0.0,
             prepare_stats.failed.load(), prepare_stats.replaced.load(),
             prepare_stats.stale.load(), stats.prepared_vertices,
             stats.prepared_vertices_rejected, stats.prepared_installed,
             [&] {
                 std::string text;
                 for (u32 i = 0; i < stats.record_ns.size(); ++i) {
                     text += fmt::format("{}{:.2f}", i == 0 ? "" : "/",
                                         stats.timed_draws
                                             ? stats.record_ns[i] / 1000.0 / stats.timed_draws
                                             : 0.0);
                 }
                 return text;
             }(),
             stats.fill_shaders, stats.rewrite_clears, stats.rewrite_uploads,
             stats.rewrite_partial, stats.rewrite_fills,
             stats.gathered_bytes / 1048576.0,
             registry->GetUploadStats().gpu.load(), registry->GetUploadStats().cpu.load(),
             registry_stats.lookups.load(),
             registry_stats.created_textures.load(), registry_stats.created_targets.load(),
             registry_stats.uploads.load(), registry_stats.upload_bytes.load() / 1048576.0,
             stats.draws / seconds, stats.draws ? stats.draw_ns / 1000.0 / stats.draws : 0.0,
             stats.dispatches ? stats.dispatch_ns / 1000.0 / stats.dispatches : 0.0,
             fmt::join(stats.null_reasons, "/"), [&] {
                 std::string text;
                 for (u32 i = 0; i < stats.phase_ns.size(); ++i) {
                     text += fmt::format(
                         "{}{:.2f}", i == 0 ? "" : (i == 4 ? "(" : (i == 5 ? ")/" : "/")),
                         stats.timed_draws ? stats.phase_ns[i] / 1000.0 / stats.timed_draws
                                           : 0.0);
                 }
                 return text;
             }());
    if (in_place.frames != 0) {
        const auto mb = [&](u32 i) { return in_place.total[i] * 64.0 / 1024.0 / in_place.frames; };
        LOG_INFO(Render_Vulkan,
                 "NAT-142: guest memory used in place per frame (64 KB pages, MB): vertices "
                 "{:.1f}, indices {:.1f}, read-only storage {:.1f}, storage read {:.1f}, storage "
                 "written {:.1f}; from video memory instead: mirrors {:.1f}, regions {:.1f} "
                 "(NAT-150; {} frames sampled)",
                 mb(0), mb(1), mb(2), mb(3), mb(4), mb(5), mb(6), in_place.frames);
        in_place.total = {};
        in_place.frames = 0;
    }
    LOG_INFO(Render_Vulkan,
        "OPT-PIPELINE-BINDS: graphics emitted={} reused={}, compute emitted={} reused={} in 10 s",
        stream.pipelines.emitted[0], stream.pipelines.reused[0],
        stream.pipelines.emitted[1], stream.pipelines.reused[1]);
    stream.pipelines.ResetCounts();
    LOG_INFO(Render_Vulkan, "OPT-DESCRIPTORS: {} identical sets reused, {} sets allocated in 10 s",
             stats.descriptor_reuses, stats.descriptor_allocations);
    {
        std::scoped_lock lock{chunk_mutex};
        u64 upload_free_bytes = 0;
        for (const auto& chunk : free_chunks) upload_free_bytes += chunk.buffer->Size();
        LOG_INFO(Render_Vulkan,
                 "VRAM-OWNERS: native descriptor_pools active={} free={} retiring={} "
                 "upload_chunks active={} free={} retiring={} images_live={} images_evicted={} "
                 "views_retiring={} upload_free_bytes={}",
                 stream.pools.size(), free_pools.size(), retiring_pools.size(),
                 stream.chunks.size(), free_chunks.size(), retiring_chunks.size(),
                 registry->Count(), registry_stats.evicted.load(), retired_views.size(), upload_free_bytes);
    }
    LOG_INFO(Render_Vulkan, "NAT: flat uploads reused={} uploaded={} bytes_saved={}",
             stats.flat_upload_hits, stats.flat_upload_misses, stats.flat_upload_bytes_saved);
    stats.flat_upload_hits = stats.flat_upload_misses = stats.flat_upload_bytes_saved = 0;
    if (ParallelEligibilityEnabled() && parallel_eligibility_owner==this) {
        auto& probe=parallel_eligibility_stats; probe.Flush();
        LOG_INFO(Render_Vulkan, "PARALLEL-ELIGIBILITY-DIAG observed={} upper_bound={} run_counts_1_2_4_8_16_32_64plus={}/{}/{}/{}/{}/{}/{} draws_per_bucket={}/{}/{}/{}/{}/{}/{}; not proof of safe worker recording",
            probe.observed,probe.eligible,probe.runs[0],probe.runs[1],probe.runs[2],probe.runs[3],probe.runs[4],probe.runs[5],probe.runs[6],
            probe.eligible_draws_in_runs[0],probe.eligible_draws_in_runs[1],probe.eligible_draws_in_runs[2],probe.eligible_draws_in_runs[3],probe.eligible_draws_in_runs[4],probe.eligible_draws_in_runs[5],probe.eligible_draws_in_runs[6]);
        LOG_INFO(Render_Vulkan,"PARALLEL-ELIGIBILITY-DIAG independent_filters race={} nonindexed={} single_color={} no_clear={} no_depth={} no_descriptors={} no_storage={} no_feedback={}",
            probe.race,probe.nonindexed,probe.single_color,probe.no_clear,probe.no_depth,probe.no_descriptors,probe.no_storage,probe.no_feedback);
        probe={};
    }
    if(auto* sidecar=FindParallelRuntime(this)) {
        for(u32 mode=0;mode<sidecar->record_partition.size();++mode) {
            const auto& p=sidecar->record_partition[mode];
            LOG_INFO(Render_Vulkan,"PARALLEL-RECORD-PARTITION mode_at_entry={} nominal_sample_rate=1/16 admission_samples={} admission_ns={} tail_samples={} tail_ns={} chunk_samples={} chunk_ns={} report_samples={} report_ns={} end_calls={} end_ns={} (end is inclusive and overlaps other scopes; chunk may include Splice/Drain; report sample completes after this log)",mode,p[0].count,p[0].wall_ns,p[1].count,p[1].wall_ns,p[2].count,p[2].wall_ns,p[3].count,p[3].wall_ns,p[4].count,p[4].wall_ns);
        }
        sidecar->record_partition={};
        LOG_INFO(Render_Vulkan,"PARALLEL-PRODUCER-CPU cumulative structure={} vertex={} snapshot={} actions={} upload_lookups={} stage_schemas={} stage_lookups={} clip_fast={} direct_table_cache_hits={}",sidecar->producer_structure_hits,sidecar->producer_vertex_hits,sidecar->producer_snapshot_hits,sidecar->producer_action_hits,sidecar->producer_upload_lookup_hits,sidecar->producer_stage_schema_hits,sidecar->producer_stage_lookup_hits,sidecar->clip_fast_hits,sidecar->direct_table_cache_hits);
        const auto& batch=sidecar->descriptor_updates;
        LOG_INFO(Render_Vulkan,"PARALLEL-DESCRIPTOR-BATCH cumulative logical_draws={} actual_calls={} staged_rows={} immediate_draws={} overflow_draws={} unsupported_draws={} high_rows={} pending_rows={} flush_end={} flush_serial={} flush_abandoned={} flush_capacity={} flush_unsupported={} flush_exception={}",
            batch.logical_draws,batch.actual_calls,batch.staged_rows,batch.immediate_draws,batch.overflow_draws,batch.unsupported_draws,batch.high_rows,batch.nw,batch.flushes[PendingDescriptorUpdates::EndPass],batch.flushes[PendingDescriptorUpdates::Serial],batch.flushes[PendingDescriptorUpdates::Abandoned],batch.flushes[PendingDescriptorUpdates::Capacity],batch.flushes[PendingDescriptorUpdates::Unsupported],batch.flushes[PendingDescriptorUpdates::Exception]);
        sidecar->FinishSerialEligibleRun();
        LOG_INFO(Render_Vulkan,"PARALLEL-SERIAL-ELIGIBLE runs_1={} runs_2_3={} runs_4_7={} runs_8plus={} max={} (serial bypass excludes unexamined draws; report boundaries split runs)",
            sidecar->serial_eligible_runs[0],sidecar->serial_eligible_runs[1],sidecar->serial_eligible_runs[2],sidecar->serial_eligible_runs[3],sidecar->serial_eligible_run_max);
        sidecar->serial_eligible_runs={};sidecar->serial_eligible_run_max=0;
        if(sidecar->whole_samples) {
            LOG_INFO(Render_Vulkan,"PARALLEL-WHOLE-PREFLIGHT sampled={} wall_ns={} max_ns={} cpu_samples={} cpu_100ns={} (1/128, enabled mode only)",
                sidecar->whole_samples,sidecar->whole_wall_ns,sidecar->whole_max_ns,sidecar->whole_cpu_samples,sidecar->whole_cpu_100ns);
            sidecar->whole_samples=sidecar->whole_wall_ns=sidecar->whole_max_ns=sidecar->whole_cpu_samples=sidecar->whole_cpu_100ns=0;
        }
        if(PreparedSampledProofRequested()) {
            LOG_INFO(Render_Vulkan,"PARALLEL-PREPARED-VIEW attempts={} accepted={} fallback={}",sidecar->prepared_view_attempts,sidecar->prepared_view_accepted,sidecar->prepared_view_fallbacks);
            sidecar->prepared_view_attempts=sidecar->prepared_view_accepted=sidecar->prepared_view_fallbacks=0;
            if(sidecar->prepared_proof_context){const auto state=sidecar->prepared_proof_context->state.load(std::memory_order_seq_cst);
                LOG_INFO(Render_Vulkan,"PARALLEL-PREPARED-VIEW-STATE mode={} generation={} active={}",bool(state&1),state>>1,sidecar->prepared_proof_context->active.load(std::memory_order_seq_cst));}
        }
        if(ParallelGateDiagEnabled()) {
            const auto top=sidecar->gates.Top();u64 shown=0;
            for(const auto& slot:top) if(slot.count) {shown+=slot.count;LOG_INFO(Render_Vulkan,"PARALLEL-GATE-DIAG mask={:#x} count={}",slot.mask,slot.count);}
            const auto& gates=sidecar->gates;
            LOG_INFO(Render_Vulkan,"PARALLEL-GATE-DIAG observed={} shown={} other={} overflow={} unstable={}",
                gates.observed,shown,gates.observed-shown-gates.overflow-gates.unstable,gates.overflow,gates.unstable);
            LOG_INFO(Render_Vulkan,"PARALLEL-GATE-LEGEND 0=race 1=shaderpatch 2=trace 3=native-view 4=alias 5=color-count 6=clear 7=depth 8=index-quad 9=composition 10=vertex-buffers 11=vertex-attributes 12=index-buffer 13=prepared-owner 14=stage-tess 15=attribute-known 16=resource-key-known 17=translation 18=fetch 19=direct-dma 20=fmask-storage-image 21=shader-clock 22=buffer-write 23=draw-write 24=descriptor-check 25=deep-unexamined 26=serial-scope-unexamined");
            LOG_INFO(Render_Vulkan,"PARALLEL-CAPTURED-VERTEX counts_0_1_2to3_4to7_8to15_16to31_32={}/{}/{}/{}/{}/{}/{} sum={} (captured draws only)",
                sidecar->captured_vertex_counts[0],sidecar->captured_vertex_counts[1],sidecar->captured_vertex_counts[2],sidecar->captured_vertex_counts[3],sidecar->captured_vertex_counts[4],sidecar->captured_vertex_counts[5],sidecar->captured_vertex_counts[6],sidecar->captured_vertex_sum);
            sidecar->captured_vertex_counts={};sidecar->captured_vertex_sum=0;
            const auto& vp=sidecar->view_proof_stats;
            LOG_INFO(Render_Vulkan,"PARALLEL-VIEW-PROOF queries={} first_hits={} index_hits={} walks={} fallbacks={} overflows={} wanted_builds={} wanted_overflows={} filtered_views={} (enabled proof only)",vp.queries,vp.first_hits,vp.index_hits,vp.walks,vp.fallbacks,vp.overflows,vp.wanted_builds,vp.wanted_overflows,vp.filtered_views);
            sidecar->view_proof_stats={};
            sidecar->gates={};
        }
        if(sidecar->ahead_context) {const auto& a=*sidecar->ahead_context;const auto batch=a.pools.GetStats();
            LOG_INFO(Render_Vulkan,"PARALLEL-PRODUCER-DESC mode={} preparing={} attempts={} ready={} used={} unused={} failures={} capacity={} allocation_calls={} allocated_sets={} spare_hits={} discarded_groups={} purged_spares={} used_sets={} unused_sets={} (cumulative; failures include busy/driver, capacity includes unsupported/cap)",
                a.mode.load(std::memory_order_acquire),a.active.load(std::memory_order_acquire),a.attempts.load(),a.ready.load(),a.used.load(),a.unused.load(),a.failures.load(),a.capacity.load(),batch.calls,batch.sets_allocated,batch.spare_hits,batch.discarded,batch.purged,a.used_sets.load(),a.unused_sets.load());}
        u64 sampler_entries=0;
        if(ParallelGateDiagEnabled()){std::scoped_lock sampler_lock{sampler_mutex};sampler_entries=samplers.size();}
        if(sidecar->view_sample.draws) {auto& v=sidecar->view_sample;const auto& q=v.proof;
            LOG_INFO(Render_Vulkan,"PARALLEL-VIEW-SAMPLE draws={} queried_draws={} image_infos={} sampled_entries={} queries={} first_hits={} index_hits={} walks={} fallbacks={} late_fallbacks={} overflows={} wanted_builds={} wanted_overflows={} filtered_views={} entries_0_1_2_4_8_16_32_64_128_129plus={}/{}/{}/{}/{}/{}/{}/{}/{}/{} (1/128 descriptor-safe draws; entry cardinality weighted by existing map lookups)",
                v.draws,v.queried_draws,v.image_infos,v.sampled_entries,q.queries,q.first_hits,q.index_hits,q.walks,q.fallbacks,q.fallbacks>=v.queried_draws?q.fallbacks-v.queried_draws:0,q.overflows,q.wanted_builds,q.wanted_overflows,q.filtered_views,
                v.cardinality[0],v.cardinality[1],v.cardinality[2],v.cardinality[3],v.cardinality[4],v.cardinality[5],v.cardinality[6],v.cardinality[7],v.cardinality[8],v.cardinality[9]);
            const auto sequence=v.sequence;v={};v.sequence=sequence;
        }
        if(ParallelProofPhaseEnabled()) {
            LOG_INFO(Render_Vulkan,"PARALLEL-PROOF-PHASE-LEGEND 0=buffer-write-group 1=sampler-descriptor 2=sampled-view-descriptor 3=push-snapshot 4=upload-lookup 5=chunk-scan 6=direct-slot 7=readonly-slot 8=clip-slot; each subphase count is per invocation (including early clip probes), wall overlaps outer phases and includes timer overhead");
            for(u32 i=0;i<sidecar->proof_phases.size();++i) {const auto& t=sidecar->proof_phases[i];
                LOG_INFO(Render_Vulkan,"PARALLEL-PROOF-PHASE phase={} samples={} wall_ns={} wall_max_ns={}",i,t.count,t.wall_ns,t.wall_max);
            }
            sidecar->proof_phases={};
        }
        const auto admission=sidecar->coordinator->TakeAdmissionDiag();
        const auto report_rejections=[&](const char* kind,const auto& v) {
            LOG_INFO(Render_Vulkan,"PARALLEL-ADMISSION-REJECT kind={} failed={} zero_epoch={} epoch_mismatch={} node_cap={} validation={} invalid={} overflow={} charge={} budget_count={} budget_bytes={}",kind,v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9]);
        };
        report_rejections("small",admission.small_rejected);report_rejections("large",admission.large_rejected);report_rejections("prefix",admission.prefix_rejected);
        LOG_INFO(Render_Vulkan,"PARALLEL-ADMISSION small_attempts={} small_accepted={} small_submit_failed={} small_exceptions={} large_attempts={} large_accepted={} large_submit_failed={} large_exceptions={} prefix_attempts={} prefix_accepted={} prefix_exceptions={} partial_drain={} partial_large={}",admission.small_attempts,admission.small_accepted,admission.small_submit_failed,sidecar->small_admission_exceptions,admission.large_attempts,admission.large_accepted,admission.large_submit_failed,sidecar->large_admission_exceptions,admission.prefix_attempts,admission.prefix_accepted,admission.prefix_exceptions,admission.partial_drain,admission.partial_large);
        const auto& budget=admission.budget;
        LOG_INFO(Render_Vulkan,"PARALLEL-BUDGET current_count={} current_bytes={} max_count={} max_bytes={} interval_peak_count={} interval_peak_bytes={}",budget.count,budget.bytes,budget.max_count,budget.max_bytes,budget.peak_count,budget.peak_bytes);
        const auto& rotation=sidecar->rotation_diag;const auto& sizes=sidecar->pass_size_hist;
        LOG_INFO(Render_Vulkan,"PARALLEL-ROTATION attempts={} success={} invalid={} preflight={} acquire={} command={} prefix={} exception={} pass_sizes_0_1_2_3_4_5_6_7_8plus={},{},{},{},{},{},{},{},{}",rotation.attempts,rotation.success,rotation.invalid,rotation.preflight,rotation.acquire,rotation.command,rotation.prefix,rotation.exception,sizes[0],sizes[1],sizes[2],sizes[3],sizes[4],sizes[5],sizes[6],sizes[7],sizes[8]);
        sidecar->rotation_diag={};sidecar->pass_size_hist={};sidecar->small_admission_exceptions=sidecar->large_admission_exceptions=0;
        const auto workers=sidecar->coordinator->TakeStats();
        if(sidecar->coordinator->TransferEnabled()) {const auto transfer=sidecar->coordinator->TakeTransferStats();
            LOG_INFO(Render_Vulkan,"PARALLEL-TRANSFER-PASSES attempts={} success={} fallback={}",transfer.success+transfer.fallback,transfer.success,transfer.fallback);}
        const auto groups=sidecar->coordinator->TakeGroupStats();
        LOG_INFO(Render_Vulkan,"PARALLEL-SERIAL-SPLIT attempts={} success={} first_draws={}",sidecar->serial_split_attempts,sidecar->serial_split_success,sidecar->serial_split_first_draws);
        sidecar->serial_split_attempts=sidecar->serial_split_success=sidecar->serial_split_first_draws=0;
        if(ParallelGateDiagEnabled()) for(unsigned reason=0;reason<3;++reason) {
            const auto& p=groups.passes[reason];const auto& d=groups.draws[reason];
            LOG_INFO(Render_Vulkan,"PARALLEL-BATCH-GROUP reason={} passes1to8={},{},{},{},{},{},{},{} draws1to8plus={},{},{},{},{},{},{},{} lead_samples={} lead_ns={} lead_max_ns={}",reason,p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],d[1],d[2],d[3],d[4],d[5],d[6],d[7],d[8],groups.lead_samples,groups.lead_ns,groups.lead_max_ns);
        }
        if(ParallelGateDiagEnabled()) {
            const auto& f=sidecar->descriptor_fail_reasons;
            LOG_INFO(Render_Vulkan,"PARALLEL-DESCRIPTOR-FAIL write={} pnext={} buffer_slice={} buffer_unproved={} image_slice={} sampler_invalid={} sampler_unknown={} image_invalid={} image_unknown={} snapshot={} samples={}",
                f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],f[8],f[9],sidecar->descriptor_samples);
            sidecar->descriptor_fail_reasons={};sidecar->descriptor_samples=0;
        }
        LOG_INFO(Render_Vulkan,"PARALLEL-RECORD-DIAG absent_sampled_attempts={} absent_sampled_accepted={} absent_sampled_rejected={} sampled_null_view={} sampled_nonnull_sampler={} sampled_nongeneral={} sampled_canonical_null={} absent_mode0_attempts={} absent_mode0_accepted={} absent_mode0_rejected={} natural_preclosed={} natural_captured={} natural_serial_fallback={} natural_binding_skip={} sampler_entries={} sampler_proof_hits={} sampler_proof_misses={} sampler_scan_steps={} sampler_scan_positives={} proof_table_builds={} proof_table_hits={} extended_clear_rejected={} fast_rejected={} no_pass_room_rejected={} serial_scope_bypassed={} owner_slot_pressure={} eligible_in_serial_scope_examined={} eligible_in_active_scope={} eligible_at_boundary={} vertex_candidates={} vertex_preflight={} vertex_backing_pass={} vertex_bad_structure={} vertex_bad_binding={} vertex_bad_pnext={} vertex_unknown_backing={} vertex_direct_ranges={} vertex_direct_attempts={} vertex_direct_matches={} descriptor_readonly_attempts={} descriptor_readonly_regions={} descriptor_readonly_imports={} descriptor_readonly_uncontained={} descriptor_readonly_unmatched={} descriptor_clip_nulls={} direct_slot_attempts={} direct_slot_accepted={} considered={} eligible={} resource_key_unknown={} buffer_present={} alias_rejected={} descriptor_rejected={} missing_sets={} preflight_samples={} preflight_wall_ns={} preflight_cpu_samples={} preflight_cpu_100ns={} passes={} draws={} queued={} synchronous={} worker_jobs={} worker_wall_ns_sum={} worker_cpu_100ns={} worker_cpu_samples={} peak_workers={} worker_recorded_passes={} worker_recorded_draws={} small_groups_submitted={} small_groups_synchronous={} small_passes={} small_submitted_draws={} drain_wall_ns={} coordinator_cpu_100ns={} coordinator_cpu_samples={} pending={} inflight={}",
            sidecar->absent_sampled_attempts,sidecar->absent_sampled_accepted,sidecar->absent_sampled_rejected,sidecar->sampled_null_view,sidecar->sampled_nonnull_sampler,sidecar->sampled_nongeneral,sidecar->sampled_canonical_null,sidecar->absent_mode0_attempts,sidecar->absent_mode0_accepted,sidecar->absent_mode0_rejected,sidecar->natural_preclosed,sidecar->natural_captured,sidecar->natural_serial_fallback,sidecar->natural_binding_skip,sampler_entries,sidecar->sampler_proof_hits,sidecar->sampler_proof_misses,sidecar->sampler_scan_steps,sidecar->sampler_scan_positives,sidecar->proof_table_builds,sidecar->proof_table_hits,sidecar->extended_clear_rejected,sidecar->fast_rejected,sidecar->no_pass_room_rejected,sidecar->serial_scope_bypassed,sidecar->owner_slot_pressure,sidecar->eligible_in_serial_scope,sidecar->eligible_in_active_scope,sidecar->eligible_at_boundary,sidecar->vertex_candidates,sidecar->vertex_preflight_draws,sidecar->vertex_backing_pass,sidecar->vertex_bad_structure,sidecar->vertex_bad_binding,sidecar->vertex_bad_pnext,sidecar->vertex_unknown_backing,sidecar->vertex_direct_ranges,sidecar->vertex_direct_attempts,sidecar->vertex_direct_matches,sidecar->descriptor_readonly_attempts,sidecar->descriptor_readonly_regions,sidecar->descriptor_readonly_imports,sidecar->descriptor_readonly_uncontained,sidecar->descriptor_readonly_unmatched,sidecar->descriptor_clip_nulls,sidecar->direct_slot_attempts,sidecar->direct_slot_accepted,sidecar->considered,sidecar->eligible,sidecar->unknown_resource_key,sidecar->buffer_rejected,sidecar->alias_rejected,sidecar->descriptor_rejected,sidecar->missing_sets,sidecar->preflight_samples,sidecar->preflight_wall_ns,sidecar->preflight_cpu_samples,sidecar->preflight_cpu_100ns,sidecar->passes,sidecar->draws,sidecar->queued,sidecar->synchronous,workers.jobs,workers.wall_ns,workers.cpu_100ns,workers.cpu_samples,workers.peak,workers.recorded_passes,workers.recorded_draws,workers.small_groups_submitted,workers.small_groups_synchronous,workers.small_passes,workers.small_submitted_draws,sidecar->drain_ns,sidecar->coordinator_cpu_100ns,sidecar->coordinator_cpu_samples,
            sidecar->coordinator->PendingNodes(),sidecar->coordinator->InFlightNodes());
        LOG_INFO(Render_Vulkan,"PARALLEL-READONLY-HINT range_hits={} range_misses={} region_hits={} region_misses={}",
            sidecar->readonly_range_hint_hits,sidecar->readonly_range_hint_misses,sidecar->readonly_region_hint_hits,sidecar->readonly_region_hint_misses);
        sidecar->readonly_range_hint_hits=sidecar->readonly_range_hint_misses=sidecar->readonly_region_hint_hits=sidecar->readonly_region_hint_misses=0;
        sidecar->absent_sampled_attempts=sidecar->absent_sampled_accepted=sidecar->absent_sampled_rejected=sidecar->sampled_null_view=sidecar->sampled_nonnull_sampler=sidecar->sampled_nongeneral=sidecar->sampled_canonical_null=sidecar->absent_mode0_attempts=sidecar->absent_mode0_accepted=sidecar->absent_mode0_rejected=sidecar->natural_preclosed=sidecar->natural_captured=sidecar->natural_serial_fallback=sidecar->natural_binding_skip=sidecar->sampler_proof_hits=sidecar->sampler_proof_misses=sidecar->sampler_scan_steps=sidecar->sampler_scan_positives=sidecar->proof_table_builds=sidecar->proof_table_hits=sidecar->extended_clear_rejected=sidecar->fast_rejected=sidecar->no_pass_room_rejected=sidecar->serial_scope_bypassed=sidecar->owner_slot_pressure=sidecar->eligible_in_serial_scope=sidecar->eligible_in_active_scope=sidecar->eligible_at_boundary=sidecar->vertex_candidates=sidecar->vertex_preflight_draws=sidecar->vertex_backing_pass=sidecar->vertex_bad_structure=sidecar->vertex_bad_binding=sidecar->vertex_bad_pnext=sidecar->vertex_unknown_backing=sidecar->vertex_direct_ranges=sidecar->vertex_direct_attempts=sidecar->vertex_direct_matches=sidecar->descriptor_readonly_attempts=sidecar->descriptor_readonly_regions=sidecar->descriptor_readonly_imports=sidecar->descriptor_readonly_uncontained=sidecar->descriptor_readonly_unmatched=sidecar->descriptor_clip_nulls=sidecar->direct_slot_attempts=sidecar->direct_slot_accepted=sidecar->passes=sidecar->draws=sidecar->queued=sidecar->synchronous=sidecar->considered=sidecar->eligible=sidecar->unknown_resource_key=sidecar->buffer_rejected=sidecar->alias_rejected=sidecar->descriptor_rejected=sidecar->missing_sets=sidecar->preflight_samples=sidecar->preflight_wall_ns=sidecar->preflight_cpu_100ns=sidecar->preflight_cpu_samples=sidecar->drain_ns=sidecar->coordinator_cpu_100ns=sidecar->coordinator_cpu_samples=0;
    }
    LOG_INFO(Render_Vulkan, "NAT: descriptor allocation calls={} reserve hits={} unused={}",
             stats.descriptor_allocate_calls, stats.descriptor_reserve_hits, stream.descriptor_reserve.SpareCount());
    if (DescriptorAllocDiagEnabled() && descriptor_alloc_owner == this) {
        const auto& m=descriptor_alloc_metrics;
        for (u32 kind=0;kind<2;++kind) LOG_INFO(Render_Vulkan,
            "DESC-ALLOC-DIAG kind={} miss-hist-0/1/2/3/4/5/6plus={}/{}/{}/{}/{}/{}/{} cache_hits={} reserve_hits={}",
            kind,m.misses[kind][0],m.misses[kind][1],m.misses[kind][2],m.misses[kind][3],m.misses[kind][4],m.misses[kind][5],m.misses[kind][6],m.cache_hits[kind],m.reserve_hits[kind]);
        for (u32 batch=0;batch<2;++batch) for(u32 attempt=0;attempt<3;++attempt) {
            const auto& t=m.timing[batch][attempt];
            LOG_INFO(Render_Vulkan,"DESC-ALLOC-DIAG batch={} attempt={} success={} capacity={} other={} samples={} wall_ns_sum={} wall_ns_max={} cpu_samples={} cpu_100ns_sum={} cpu_100ns_max={}",
                batch,attempt,m.results[batch][attempt][0],m.results[batch][attempt][1],m.results[batch][attempt][2],t.samples,t.wall_ns,t.wall_max_ns,t.cpu_samples,t.cpu_100ns,t.cpu_max_100ns);
        }
        LOG_INFO(Render_Vulkan,"DESC-ALLOC-DIAG pool_initial={} pool_rotation={} spare={}",m.pool_initial,m.pool_rotate,stream.descriptor_reserve.SpareCount());
        for (u32 kind=0;kind<2;++kind) LOG_INFO(Render_Vulkan,
            "DESC-BIND-DIAG kind={} sets={} runs={} multi_runs={}",
            kind,descriptor_bind_sets[kind],descriptor_bind_runs[kind],descriptor_bind_multi[kind]);
        std::fill_n(descriptor_bind_sets, 2, 0); std::fill_n(descriptor_bind_runs, 2, 0); std::fill_n(descriptor_bind_multi, 2, 0);
        descriptor_alloc_metrics={};
    }
    stats.descriptor_allocate_calls = stats.descriptor_reserve_hits = 0;
    stats.descriptor_reuses = stats.descriptor_allocations = 0;
    LOG_INFO(Render_Vulkan, "DESC-HASH: {} prepared hashes reused, {} refreshed after buffer changes",
             descriptor_hash_reused, descriptor_hash_refreshed);
    descriptor_hash_reused = descriptor_hash_refreshed = 0;
    const u64 cpu_writes = stats.cpu_writes.load();
    stats.draws = stats.dispatches = stats.skipped = stats.no_pipeline = stats.passes = 0;
    stats.barriers = stats.splices = stats.copied_reads = stats.copied_bytes = stats.unbound = 0;
    stats.null_images = stats.fills = stats.copies = stats.meta_clears = stats.inline_writes = 0;
    stats.source_copies = stats.draw_ns = stats.dispatch_ns = 0;
    stats.timed_draws = 0;
    stats.texture_memo_hits = stats.texture_lookups = stats.buffer_memo_hits = 0;
    stats.vertex_near = stats.vertex_skipped = 0;
    stats.prepared_used = stats.prepared_missing = 0;
    stats.prepared_rejected = {};
    stats.prepared_vertices = stats.prepared_vertices_rejected = 0;
    stats.prepared_installed = 0;
    stats.record_ns = {};
    prepare_stats.made = 0;
    prepare_stats.failed = 0;
    prepare_stats.replaced = 0;
    prepare_stats.stale = 0;
    prepare_stats.ns = 0;
    stats.gathers = stats.gathered_bytes = stats.writebacks = 0;
    stats.region_binds = stats.regions_made = stats.region_refreshes = 0;
    stats.fill_shaders = stats.rewrite_clears = stats.rewrite_uploads = 0;
    stats.rewrite_partial = stats.rewrite_fills = 0;
    stats.pass_barriers = stats.passes_unblocked = 0;
    stats.region_refresh_pages = stats.region_refresh_ns = 0;
    stats.guest_buffer_calls = stats.guest_buffer_ns = 0;
    stats.mirror_binds = stats.mirrors_made = stats.mirror_made_bytes = stats.mirrors_dropped = 0;
    stats.mirrors_evicted = stats.mirror_over_budget = stats.mirror_refresh_pages = 0;
    stats.null_reasons = {};
    stats.phase_ns = {};
    (void)cpu_writes;
}

} // namespace Vulkan

#include "video_core/renderer_vulkan/prt_view_selftest.h"
#include "video_core/renderer_vulkan/prt_view_gpu_test.h"
