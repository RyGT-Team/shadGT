#include "video_core/renderer_vulkan/page_coverage.h"
#include "video_core/renderer_vulkan/page_watch_tracker.h"
#include "video_core/renderer_vulkan/descriptor_set_reserve.h"
#include "video_core/renderer_vulkan/prepared_descriptor_key.h"
#include "video_core/renderer_vulkan/optional_mirror_budget.h"
#include "ryty_gpu/native/prepared_sampled_view_proof.h"
#include "ryty_gpu/native/prepared_proof_context.h"
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// NAT-100: native mode (RYGT_NATIVE=1, ryGT builds); see vk_rasterizer_native.cpp.

#pragma once
#include "video_core/renderer_vulkan/pipeline_owner.h"

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/container/static_vector.hpp>

#include "common/types.h"
#include "video_core/renderer_vulkan/binding_command_memo.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/cb_db_extent.h"
#include "ryty_gpu/native/same_texel.h"
#include "ryty_gpu/native/retired_snapshot_pool.h"
#include "ryty_gpu/native/writeback_plan.h"
#include "video_core/renderer_vulkan/pipeline_binding_state.h"
#include "video_core/renderer_vulkan/descriptor_set_memo.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/regs_texture.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h" // SO-010

namespace AmdGpu {
union Regs;
}

namespace Shader {
struct Info;
}

namespace VideoCore {
class Sampler;
class DirectMemory;
}

namespace Native {
class ImageEntry;
class ImageRegistry;
struct RememberedFillReceipt;
struct MadeImage;
namespace Rhi {
class Buffer;
class Image;
class CommandList;
class Device;
class Fence;
class Pipeline;
class PipelineLayout;
} // namespace Rhi
} // namespace Native

namespace Vulkan {
class OneOffObserver;

template <class Buffer> class AsyncMirrorBuffer;

class Rasterizer;
struct GtsSceneImage;
class GraphicsPipeline;
class ComputePipeline;

/// NAT-100: whether native mode was asked for (RYGT_NATIVE=1).
bool NativeModeRequested();

class NativeRenderer {
public:
    explicit NativeRenderer(Rasterizer& rasterizer);
    ~NativeRenderer();

    NativeRenderer(const NativeRenderer&) = delete;
    NativeRenderer& operator=(const NativeRenderer&) = delete;

    // PERF-GT7-008: prepare a direct list when the serialized recorder goes idle.
    void BeginAhead();

    void Draw(bool is_indexed, u32 index_offset);
    void DispatchDirect();
    void DispatchIndirect(VAddr address, u32 offset, u32 size);
    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds);
    /// WRITE_DATA in command order: later work on the GPU reads the new value. False when the
    /// address can't be written by the GPU.
    bool InlineWrite(VAddr address, std::span<const u8> data);
    /// Guest memory is unmapped: textures over it are uploaded again when next used.
    void OnUnmap(VAddr address, u64 size);
    /// A successful mapping replaced the old guest pages at this address.
    void OnNewMapping(VAddr address, u64 size);
    /// NAT-105: the CPU wrote watched memory (a page fault on any thread): the textures there
    /// are uploaded again before their next use, and their pages are no longer watched.
    void OnCpuWrite(VAddr address, u64 size, u64 exact);
    void OnCpuStack(VAddr address, u64 size);
    u32 ReadGds(u32 gds_offset);

    /// Ends the command list recorded so far and runs it in the scheduler's current command
    /// buffer (called before anything else is recorded there or it is submitted).
    void Splice();

    /// The picture at a display buffer, for the presenter (none when nothing drew there).
    struct Display {
        vk::Image image;
        vk::ImageView view;
        u32 width;
        u32 height;
#ifndef NDEBUG
        vk::Format capture_format{};
        bool capture_transfer_src{};
#endif
        vk::ImageView output_view{};
        vk::ImageView ui_layer{};
        vk::Extent2D output_size{};
        bool dlss_scaled = false;
        std::string upscaler;
    };
    std::optional<Display> FindDisplay(const VideoCore::ImageInfo& info, vk::Format view_format,
                                       vk::Extent2D output_size);

    /// NAT-116: saves the render targets noted while the rasterizer collected a frame.
    void DumpFrame(const std::filesystem::path& dir);

    /// OPT-010 (RYGT_NATIVE_PREPARE=1): whether game job threads prepare draws.
    static bool PrepareEnabled();
    /// OPT-012: on the command thread, whether a job thread prepared the draw at the packet
    /// (with its stages): then the command thread doesn't select its pipeline.
    bool HasPrepared(const void* packet);
    /// OPT-010: on a game job thread whose walk reached a draw (`regs` its state there) and
    /// found its built pipeline, the stage infos holding the draw's user data: the draw's
    /// bindings worked out ahead for the recorder (see PreparedDraw).
    void PrepareJobDraw(const void* packet, u32 epoch, const AmdGpu::Regs& regs,
                        const GraphicsPipeline& pipeline,
                        std::shared_ptr<const void> pipeline_keepalive,
                        std::span<const Shader::Info* const> stages, bool is_indexed,
                        u32 index_offset);

private:
    enum class ViewUse : u8 { Sampled, Storage, Attachment, CubeSampled };

    struct Chunk {
        std::unique_ptr<Native::Rhi::Buffer> buffer;
        u64 retire_tick = 0;
    };

    /// What a draw renders into (the scheduler's RenderState, in the general layout).
    struct Targets {
        RenderState state{};
        std::array<const Native::ImageEntry*, 8> colors{};
        std::array<vk::Format, 8> color_formats{};
        const Native::ImageEntry* depth = nullptr;
    };

    struct WritebackPlan;
    /// The bindings of one draw or dispatch.
    struct DrawBindings {
        Pipeline::DescriptorWrites writes;
        Pipeline::WriteSets write_sets; ///< SHD-002: the set of each write
        std::array<std::vector<vk::WriteDescriptorSet>, Shader::Backend::MaxDescriptorSets> grouped_storage;
        std::array<std::span<const vk::WriteDescriptorSet>, Shader::Backend::MaxDescriptorSets> grouped_writes{};
        bool groups_ready = false;
        void PrepareWriteGroups();
        std::array<PreparedDescriptorKey, Shader::Backend::MaxDescriptorSets> descriptor_keys;
        vk::PipelineLayout key_layout{};
        void PrepareDescriptorKeys(const Pipeline& pipeline);
        boost::container::static_vector<vk::DescriptorBufferInfo, Shader::NUM_BUFFERS> buffers;
        boost::container::static_vector<vk::DescriptorImageInfo, Shader::NUM_IMAGES> images;
        Shader::PushData push{};
        std::vector<const Native::ImageEntry*> sampled;
        std::vector<const Native::ImageEntry*> stored;
        std::vector<const Native::ImageEntry*> image_entries;
        /// NAT-119: guest memory the draw or dispatch writes through storage buffers.
        boost::container::small_vector<std::pair<VAddr, u64>, 8> gpu_writes;
        /// NAT-120: the dispatch is GT Sport's fill shader: what it fills, with what.
        std::optional<std::array<u32, 2>> fill_pattern;
        std::optional<std::array<u32, 3>> fill_arguments_snapshot;
        VAddr fill_address = 0;
        u64 fill_size = 0;
        bool writes_buffers = false;
        bool failed = false;
        std::shared_ptr<WritebackPlan> region_writeback;
        void Clear();
    };

    struct LdsReader;
    /// The command list being recorded and its state.
    struct Stream {
        std::unique_ptr<Native::Rhi::CommandList> wrapped_list;
        Native::Rhi::CommandList* list = nullptr;
        /// PERF-GT7-018: into the command thread's ring for a list it records (RingOn).
        CmdBuf cmd{};
        bool rendering = false;
        RenderState render{};
        DynamicState dynamic{};
        PipelineBindingState<vk::Pipeline> pipelines;
        bool so_known{};
        u64 so_epoch{};
        GraphicsPipeline::ShaderObjectState so_state{};
        std::array<vk::ShaderEXT, 4> so_shaders{};
        BindingCommandMemo<vk::PipelineLayout, vk::DescriptorSet, Shader::PushData,
                           Shader::Backend::MaxDescriptorSets> binding_commands;
        /// OPT-005: the vertex input layout, vertex buffers and index buffer last recorded in
        /// this list (the same ones are not recorded again).
        bool vertex_state_known = false;
        boost::container::static_vector<vk::VertexInputBindingDescription2EXT, 32> input_bindings;
        boost::container::static_vector<vk::VertexInputAttributeDescription2EXT, 32> attributes;
        boost::container::static_vector<vk::Buffer, 32> vertex_buffers;
        boost::container::static_vector<vk::DeviceSize, 32> vertex_offsets;
        boost::container::static_vector<vk::DeviceSize, 32> vertex_sizes;
        boost::container::static_vector<vk::DeviceSize, 32> vertex_strides;
        vk::Buffer index_buffer{};
        u64 index_offset = 0;
        vk::IndexType index_type = vk::IndexType::eUint32;
        std::vector<const Native::ImageEntry*> written; ///< since the last barrier
        /// PERF-GT7-043: `foreign` says something else than a dispatch wrote them.
        struct WrittenFlag {
            bool value = false;
            bool foreign = false;
            WrittenFlag& operator=(bool written) {
                value = foreign = written;
                return *this;
            }
            WrittenFlag& operator|=(bool written) {
                value |= written;
                foreign |= written;
                return *this;
            }
            operator bool() const {
                return value;
            }
        };
        WrittenFlag buffers_written;                    ///< by shaders since the last barrier
        /// PERF-GT7-043: the program of the last dispatch, if it is the last command so far.
        u64 run_program = 0;
        u32 run_commands = 0;
        /// PERF-GT7-057: what the run's same-texel dispatches stored since the last barrier:
        /// (picture, mip << 32 | layer), what they sampled elsewhere (FIX-GT7-087), or that one
        /// stored without a key (FIX-GT7-081).
        Native::SameTexelRun run_texels;
        u64 resource_recording_serial = 0;
        /// NAT-140: attachments drawn since the last pass or global barrier, and pictures
        /// sampled since the last global barrier: what a new pass has to wait for.
        std::vector<const Native::ImageEntry*> attach_dirty;
        std::vector<const Native::ImageEntry*> read;
        std::vector<Chunk> chunks;
        u64 chunk_used = 0;
        u64 upload_incarnation = 1;
        // Flattened user data is read-only. Keep the last immutable upload for each
        // stage until this command list retires; compare bytes, never just addresses.
        struct FlatUpload {
            std::vector<u8> bytes; // CPU-cached reference; never read back upload memory
            vk::DescriptorBufferInfo binding{};
        };
        std::array<FlatUpload, Shader::MaxStageTypes> flat_uploads{};
        DescriptorSetMemo<vk::DescriptorSet> descriptor_sets;
        DescriptorSetReserve<u64, vk::DescriptorSet> descriptor_reserve;
        std::vector<vk::DescriptorPool> pools; ///< for pipelines without push descriptors
        std::vector<std::unique_ptr<Native::Rhi::Buffer>> keep; ///< alive until the list ran
        std::vector<std::shared_ptr<void>> imported_view_leases;
        std::vector<std::shared_ptr<void>> writeback_plan_leases;
        std::vector<std::unique_ptr<Native::Rhi::Buffer>> pooled_snapshots;
        std::vector<std::shared_ptr<Native::RememberedFillReceipt>> remembered_fill_receipts;
        std::vector<LdsReader> lds_readers;
        u64 lds_bytes = 0;
        u32 commands = 0;
    };

    struct ViewRecord {
        VideoCore::ImageViewInfo info;
        ViewUse use;
        vk::UniqueImageView view;
    };

    Native::MadeImage MakeImage(const VideoCore::ImageInfo& info, bool render_target);
    vk::ImageView View(const Native::ImageEntry& entry, const VideoCore::ImageViewInfo& info,
                       ViewUse use);
    vk::Sampler SamplerFor(const AmdGpu::Sampler& sharp, AmdGpu::BorderColorBuffer border,
                           bool is_depth);
    vk::Sampler MakeSampler(const AmdGpu::Sampler& sharp, AmdGpu::BorderColorBuffer border,
                            bool is_depth);
    size_t TextureMemoSlot(const std::array<u64, 4>& raw_sharp, u32 flags) const;

    void AcquireList(Stream& s);
    bool RingEnabled();
    void ApplyRingPriority(bool enabled);
    CmdBuf CmdOf(Native::Rhi::CommandList& list) const;
    Stream& Begin();
    std::chrono::steady_clock::time_point ring_ab_start{};
    u64 ring_ab_window{};
    bool command_priority_boosted{};
    void EndRendering(u32 reason = 0);
    bool ContinueSmallParallelPass(const RenderState& target, u32 reason, bool safe);
    void BeginRendering(const RenderState& state);
    void GlobalBarrier();
    /// NAT-140: attachment writes before it finish before attachment access after it (a pass
    /// drawing into what an earlier pass drew), without waiting for anything else.
    void PassBarrier();
    void PrepareImages();
    const Native::ImageEntry* CropDisplay(const Native::ImageEntry& entry, vk::Extent2D visible);
    void CopyDisplayCrop(const Native::ImageEntry& source, Native::ImageEntry& crop,
                         vk::Extent2D visible);
    void ChunkFlushCheck();

    /// Upload memory of the stream's list: (buffer, offset, CPU pointer).
    std::tuple<vk::Buffer, u64, u8*> Allocate(u64 size, u64 align, bool* backing_proven = nullptr);
    Chunk TakeChunk(u64 size);
    void TrimIdleChunks();
    vk::DescriptorBufferInfo UploadFlatData(u32 stage, const u8* source, u32 size, bool* backing_proven = nullptr);
    vk::DescriptorPool TakeDescriptorPool();

    /// NAT-114: a range shaders write that no single imported view holds lives in a buffer of
    /// its own on the GPU (a region), filled from guest memory when made; every later binding
    /// over it uses the region, and CPU writes to its pages are copied in before the next use.
    struct Region {
        VAddr begin = 0;
        VAddr end = 0;
        std::unique_ptr<Native::Rhi::Buffer> buffer;
        std::vector<VAddr> dirty_pages; ///< written by the CPU since the last use
        std::unique_ptr<std::unordered_map<VAddr, u64>> cpu_pages = std::make_unique<std::unordered_map<VAddr, u64>>(); ///< last refresh after a CPU write
        std::unique_ptr<std::unordered_set<VAddr>> gpu_pages = std::make_unique<std::unordered_set<VAddr>>(); ///< recently CPU-written pages changed by GPU work
        bool dirty_sorted = true;
    };
    std::map<VAddr, Region> regions; ///< by begin; guarded by mutex
    u64 regions_taken_in{}; ///< recorder: incremented whenever region storage is replaced
    std::mutex region_dirty_mutex;   ///< pending_region_writes (CPU write faults: any thread)
    std::vector<std::pair<VAddr, VAddr>> pending_region_writes;
    std::vector<std::pair<VAddr, VAddr>> pending_stack_retirements;
    std::atomic<bool> has_pending_region_writes{false};
    /// FIX-173: region pages as guest memory held them before the CPU's first write since they
    /// were last copied in (taken at the write fault; guarded by region_dirty_mutex).
    std::unordered_map<VAddr, std::unique_ptr<std::array<u8, 4096>>> page_snapshots;
    std::unordered_map<VAddr, u64> cpu_recent_pages; ///< under region_dirty_mutex
    std::unordered_set<VAddr> ahead_pages; ///< under region_dirty_mutex
    std::unordered_set<VAddr> remapped_pages; ///< complete newly mapped pages awaiting refresh
    std::unordered_map<VAddr, u64> fault_chunks; ///< under region_dirty_mutex
    std::unordered_map<VAddr, std::vector<std::pair<VAddr, VAddr>>> downloaded_ranges;
    void FaultAhead(VAddr begin, VAddr end);
    struct WrittenBefore {
        VAddr address{}, begin{}, end{};
        u64 size{};
        std::unique_ptr<Native::Rhi::Buffer> copy;
        Native::Rhi::Buffer* recorded_copy{};
    };
    struct WritebackPlan {
        struct PageMark {
            VAddr region_begin{};
            std::unordered_set<VAddr>::node_type node;
        };
        struct Part {
            VAddr region_begin{}, page{};
            u64 region_offset{}, before_offset{}, guest_offset{}, size{};
            Native::Rhi::Buffer* region_buffer{};
            std::unique_ptr<Native::Rhi::Buffer> guest;
            std::shared_ptr<void> lease;
            std::size_t before_index{};
        };
        std::vector<WrittenBefore> before;
        std::vector<Part> parts;
        std::vector<std::pair<VAddr, VAddr>> forget_pages;
        bool skipped_listed{};
        u64 region_generation{};
        std::vector<PageMark> marks;
        bool shader{};
    };
    bool PrepareRegionWriteback(WritebackPlan& plan,
        std::span<const std::pair<VAddr, u64>> writes, bool shader);
    bool PrepareRegionWritebackRange(WritebackPlan& plan,
        std::span<const std::pair<VAddr, u64>> writes, bool shader);
    void RecordRegionSnapshots(const std::shared_ptr<WritebackPlan>& plan);
    void RecordRegionWriteback(const std::shared_ptr<WritebackPlan>& plan);
    void RetainRegionPlan(const std::shared_ptr<WritebackPlan>& plan);
    bool RegionPlanValid(const WritebackPlan& plan) const;
    void ValidateRegionPlan(const WritebackPlan& plan);
    void ApplyRegionWritebackFallback(WritebackPlan& plan);
    std::unique_ptr<Native::Rhi::PipelineLayout> writeback_layout;
    std::unique_ptr<Native::Rhi::Pipeline> writeback_pipeline;
    std::atomic<u32> logged_partial{};
    void SortRegionWrites();
    bool HasImageBacking(VAddr address, u64 size);
    Region* RegionOver(VAddr address, u64 size);
    Region* MakeRegion(VAddr address, u64 size, bool write);
    void RefreshRegion(Region& region, VAddr address, u64 size);
    /// NAT-150: copies in video memory of guest memory the GPU reads as vertices and indices
    /// and the CPU leaves alone (meshes), instead of reading it over PCIe at every draw. A range
    /// bound in a second frame is copied at the start of the next list; CPU writes to its pages
    /// are copied in before its next use; 64 KiB granules the CPU writes in two frames (rings of
    /// per-frame data) are read in place again for a while. Mirrors never overlap regions or
    /// each other. RYGT_NATIVE_MIRRORS=0 turns them off; RYGT_NATIVE_MIRROR_MB is the budget.
    struct Mirror {
        VAddr begin = 0;
        VAddr end = 0;
        std::unique_ptr<Native::Rhi::Buffer> buffer;
        std::vector<VAddr> dirty_pages; ///< written by the CPU (or written back) since copied
        u64 used_frame = 0;
        std::vector<u64> hashes; ///< FIX-176: guest memory per 64 KB granule when last verified
    };
    static constexpr u64 MirrorGranule = u64{64} * 1024; ///< FIX-176
    VAddr mirror_verify_cursor = 0;            ///< FIX-176
    static u64 MirrorVerifyBudget();
    static bool MirrorKind(char kind); ///< FIX-177
    /// FIX-179 (diagnosis): a vertex range bound from a mirror, copied out with guest memory.
    struct MirrorCheck {
        std::unique_ptr<Native::Rhi::Buffer> buffer;
        u64 tick;
        u64 size;
        VAddr base;
        u64 frame;
        VAddr mirror_begin;
        u64 mirror_size;
        u64 mirror_used;
        u64 dirty;
    };
    std::vector<MirrorCheck> mirror_checks;
    u64 mirror_check_counter = 0;
    void CheckMirrorBind(Mirror& mirror, VAddr base, u64 size);
    /// FIX-181 (diagnosis): how far a draw reads past its vertex V#s and their mirrors.
    void CheckVertexReach(const GraphicsPipeline& pipeline, const AmdGpu::Regs& regs,
                          bool is_indexed, u32 index_offset);
    u64 reach_check_counter = 0;
    void VerifyMirrors();
    struct MirrorCandidate {
        VAddr end = 0;
        u64 frame = 0; ///< when last considered
    };
    std::map<VAddr, Mirror> mirrors; ///< by begin; guarded by mutex
    u64 mirror_bytes = 0;
    std::unordered_map<VAddr, MirrorCandidate> mirror_candidates; ///< by page of the range start
    std::vector<std::pair<VAddr, VAddr>> pending_mirrors;          ///< made at a list's start
    VAddr mirror_promotion_resume = 0; ///< fairness hint for merged pending ranges
    OptionalMirrorBudget mirror_promotion_budget; ///< shared across lists of this frame
    std::unordered_map<u64, u64> mirror_writes;    ///< granule -> frame the CPU wrote a mirror there
    std::unordered_map<u64, u64> dynamic_granules; ///< granule -> frame it was found CPU-written
    /// The first mirror that may overlap memory from `begin` on.
    std::map<VAddr, Mirror>::iterator FirstMirror(VAddr begin);
    /// The mirror holding all of the range, if any.
    Mirror* MirrorHolding(VAddr address, u64 size);
    bool IsDynamic(VAddr begin, VAddr end, u64* blocked_granule = nullptr, u64* blocked_age = nullptr);
    std::optional<std::pair<vk::Buffer, u64>> MirrorBuffer(VAddr address, u64 size);
    bool RefreshMirror(Mirror& mirror);
    void MakePendingMirrors();
    void DropMirrors(VAddr address, u64 size, u32 diagnostic_reason = ~u32{0});
    void NoteMirrorDiagnostic(u32 reason,u64 requested_begin,u64 requested_end,
                              u64 expanded_begin,u64 expanded_end,u64 generation,
                              u64 granule = ~u64{0},u64 age = ~u64{0});
    /// NAT-150: what the last GuestBuffer bound (for the NAT-142 counters).
    enum class Bound : u8 { InPlace, Region, Mirror };
    Bound last_bound = Bound::InPlace;
    /// OPT-003: what a guest range resolved to the last time (a region, a mirror, or imported
    /// guest memory read in place), valid while no region or mirror was made or dropped and no
    /// memory unmapped since (buffer_generation); a region or mirror with pages to copy in takes
    /// the full lookup.
    struct BufferMemo {
        VAddr address = 0;
        u64 size = 0;
        u32 flags = 0;
        u64 generation = ~u64{0};
        u64 hard_generation = 0;
        u64 log_seq = 0;
        vk::Buffer buffer{};
        u64 offset = 0;
        Region* region = nullptr;
        Mirror* mirror = nullptr;
        Bound bound = Bound::InPlace;
    };
    std::array<BufferMemo, 16384> buffer_memos{}; ///< OPT-006: 2048 thrashed
    u64 buffer_generation = 0;
    u64 buffer_hard_generation = 0;
    /// OPT-003: what the last lookup can be remembered as (set by GuestBufferLookup).
    enum class MemoKind : u8 { None, Region, Mirror, InPlace };
    MemoKind memo_kind = MemoKind::None;
    Region* memo_region = nullptr;
    Mirror* memo_mirror = nullptr;
    /// OPT-004: the mirror the last GuestBuffer bound from (none when it bound anything else).
    Mirror* last_mirror = nullptr;
    /// FIX-170 (diagnosis): RYGT_NATIVE_SNAPSHOT_BELOW, 0 when off.
    static u64 SnapshotBelow();
    /// A copy of the range as it is now (NAT-106 zeros where the CPU can't read).
    std::optional<std::pair<vk::Buffer, u64>> SnapshotRange(VAddr address, u64 size);
    std::optional<std::pair<vk::Buffer, u64>> GuestBufferLookup(VAddr address, u64 size, bool write,
                                                                bool mirror);
    /// OPT-002: what a T# resolved to the last time (picture and views), by the T# and how the
    /// shader reads it, valid while the registry made or dropped no picture and no memory was
    /// unmapped since.
    struct TextureMemo {
        std::array<u64, 4> sharp{};
        u32 flags = 0;
        u64 generation = ~u64{0};
        Native::ImageEntry* entry = nullptr;
        std::array<vk::ImageView, 4> views{};
        u64 registry_generation = 0;
        u64 unmap_generation = 0;
        VAddr range_begin = 0;
        VAddr range_end = 0;
    };
    std::array<TextureMemo, 16384> texture_memos{}; ///< OPT-006
    std::atomic<u64> unmap_generation{0}; ///< (OPT-010: read by job threads)
    /// NAT-107: a guest range gathered on the GPU from the imported views holding it, into a
    /// buffer of the list (none when part of it is not imported).
    std::optional<std::pair<vk::Buffer, u64>> GatherOnGpu(VAddr address, u64 size);
    /// A guest range as a buffer the GPU can bind: imported guest memory read and written in
    /// place, else (reads only) a copy in upload memory.
    /// `mirror`: read-only data worth a copy in video memory (NAT-150: vertices, indices).
    std::optional<std::pair<vk::Buffer, u64>> GuestBuffer(VAddr address, u64 size, bool write,
                                                          bool mirror = false);

    /// OPT-013: the targets the previous draw resolved, by the registers that name them, while
    /// the registry made or dropped no picture and no memory was unmapped. Clears, rewrites,
    /// depth twins and last uses are still done at every draw.
    struct TargetsMemo {
        bool valid = false;
        u64 generation = 0;
        u32 mrt_mask = 0;
        u32 color_mode = 0;
        u32 target_mask = 0;
        u32 num_colors = 0;
        std::array<AmdGpu::ColorBuffer, 8> color_buffers{};
        std::array<AmdGpu::CbDbExtent, 8> cb_extents{};
        bool depth_used = false;
        u32 depth_flags = 0; ///< depth and stencil enable, depth write enable
        AmdGpu::DepthBuffer depth_buffer{};
        AmdGpu::DepthView depth_view{};
        VAddr htile = 0;
        AmdGpu::CbDbExtent db_extent{};
        std::array<VAddr, 8> cmasks{};
        bool htile_meta = false; ///< the depth image has HTILE (FIX-156)
        Targets targets{};
    } targets_memo;
    bool TargetsFromMemo(const AmdGpu::Regs& regs, const GraphicsPipeline& pipeline, Targets& out);
    bool ResolveTargets(const AmdGpu::Regs& regs, const GraphicsPipeline& pipeline, Targets& out);
    bool PrepareUiTarget(Targets& targets, const GraphicsPipeline& pipeline);
    GtsSceneImage TemporalImage(const Native::ImageEntry& entry);
    void CaptureNativeFsr(const GraphicsPipeline& pipeline, const Targets& targets,
                          const DrawBindings& bindings);
    bool BeginNativeFsr(const GraphicsPipeline& pipeline, const Targets& targets,
                        const DrawBindings& bindings);
    void FinishNativeFsr(const Targets& targets);
    struct FsrComposition;
    std::unique_ptr<FsrComposition> fsr_composition;
    u64 fsr_display_frame = ~u64{};
    VAddr fsr_display_address{};
    std::string fsr_display_backend;

    void ResolveUiDisplay(VAddr address, bool keep);
    void SyncUiReads(std::span<const Shader::Info* const> stages);
    void SyncUiRange(VAddr address, u64 size, bool keep);
    void BindStages(std::span<const Shader::Info* const> stages, const AmdGpu::Regs& regs,
                    bool compute, DrawBindings& b);
    void BindStagesStable(std::span<const Shader::Info* const> stages, const AmdGpu::Regs& regs,
                          bool compute, DrawBindings& b);
    void BindBuffers(const Shader::Info& stage, const AmdGpu::Regs& regs, bool compute,
                     Shader::Backend::Bindings& binding, DrawBindings& b);
    void BindTextures(const Shader::Info& stage, const AmdGpu::Regs& regs,
                      Shader::Backend::Bindings& binding, DrawBindings& b);
    void PushBindings(const Pipeline& pipeline, vk::PipelineBindPoint bind_point,
                      DrawBindings& b);
    /// A draw's vertex and index buffers: looked up before its pass starts (a lookup may gather
    /// on the GPU, outside rendering) and bound inside it.
    struct VertexIndexState {
        boost::container::static_vector<vk::VertexInputAttributeDescription2EXT, 32> attributes;
        boost::container::static_vector<vk::VertexInputBindingDescription2EXT, 32> input_bindings;
        boost::container::static_vector<vk::Buffer, 32> buffers;
        boost::container::static_vector<vk::DeviceSize, 32> offsets;
        boost::container::static_vector<vk::DeviceSize, 32> sizes;
        boost::container::static_vector<vk::DeviceSize, 32> strides;
        vk::Buffer index_buffer{};
        u64 index_offset = 0;
        vk::IndexType index_type = vk::IndexType::eUint32;
        u32 index_count = 0;
    };
    /// PERF-GT7-060: a FIX-227 draw's reader pass, recorded after its run's barrier: what it
    /// binds and draws.
    struct LdsReader {
        const GraphicsPipeline* pipeline = nullptr;
        std::shared_ptr<const void> pipeline_owner;
        DynamicState dynamic{};
        VertexIndexState vertices{};
        DrawBindings bindings;
        std::vector<std::pair<u32, u32>> infos;
        u64 address = 0;       ///< its scratch
        const Native::Rhi::Buffer* scratch = nullptr; // owned by stream.keep
        u64 tail_offset = 0, tail_size = 0;
        bool indexed = false;
        u32 count = 0, instances = 0, vertex_offset = 0, first_instance = 0;
    };
    /// PERF-GT7-060 (on; RYGT_NATIVE_LDS_BATCH=0 off): the FIX-227 draws of a
    /// run share one barrier.
    static bool LdsBatchOn();
    static bool CaptureLdsReader(LdsReader& reader, const DrawBindings& bindings);
    void BindGraphics(Stream& s, const GraphicsPipeline& pipeline,
                      const std::shared_ptr<const void>& owner);
    /// PERF-GT7-060: the stream's waiting reader passes, after one full barrier, in their pass
    /// (reason: 0 another draw, 1 the pass ends, 2 the run's limit).
    void FlushLdsReaders(u32 reason);
    void ResolveVertexBuffers(const GraphicsPipeline& pipeline, const AmdGpu::Regs& regs,
                              VertexIndexState& v);
    bool ResolveIndices(const AmdGpu::Regs& regs, bool is_indexed, u32 index_offset,
                        bool quad_triangles, VertexIndexState& v);
    void RecordVertexIndex(const VertexIndexState& v);
    bool NeedsBarrierFor(const DrawBindings& b) const;
    bool RunNeedsBarrier(const Stream& s, const DrawBindings& b, u64 program, bool recorded,
                         std::optional<u64> texel = std::nullopt) const;
    void NoteWrites(const Targets* targets, const DrawBindings& b);
    /// NAT-105: CPU writes to the pages of a range fault (once) from now on.
    void WatchPages(VAddr address, u64 size);
    /// NAT-105, FIX-153: the watched pages of [begin, end) (page aligned) are watched no more.
    void UnwatchPages(VAddr begin, VAddr end);
    /// Retired resources must not leave write protection on memory reused by the CPU.
    std::vector<std::pair<VAddr, VAddr>> retired_watch_ranges;
    void ReleaseUnusedWatches();
    /// NAT-112: small render targets drawn into are written back to guest memory (GT Sport's
    /// CPU reads its brightness chain there for auto exposure).
    void FlushWritebacks();
    /// FIX-156 (diagnosis): reads back the pictures at RYGT_NATIVE_WATCH_TEXTURE now and then.
    void ProbeWatchedPictures();
    /// FIX-157 (diagnosis): a T# at a watched address and the picture it resolved to.
    void WatchBind(const Native::ImageEntry& entry, const AmdGpu::Image& tsharp, bool storage,
                   bool memo, u64 program);
    struct Probe {
        std::unique_ptr<Native::Rhi::Buffer> buffer;
        u64 tick;
        u64 size;
        u64 frame;
        u64 version;
        u32 width, height;
        bool target;
        VAddr address;
        bool packed_r11g11b10 = false; // FIX-171
        u32 mips = 0, layers = 0;      // FIX-171: an untiled copy of every mip and layer
    };
    std::vector<Probe> probes;
    /// FIX-175 (diagnosis): rectangles of presented pictures on their way back from the GPU.
    std::vector<Probe> display_probes;
    std::array<double, 3> display_last_part{}; ///< FIX-182
    std::vector<u8> display_last_bytes;        ///< FIX-182
    u32 display_glitch_dumps = 0;              ///< FIX-182
    void ProbeDisplay(const Native::ImageEntry& entry);
    /// FIX-157: bytes of pictures kept past their idle time.
    u64 kept_bytes = 0;
    std::vector<Native::ImageEntry*> writebacks;
    struct PendingLinearDownload;
    std::unordered_map<const Native::ImageEntry*, std::shared_ptr<PendingLinearDownload>> pending_linear_downloads;
    // Caller holds the renderer mutex; a mapping change cancels only overlapping plans.
    void CancelLinearDownloads(VAddr address, u64 size);
    std::vector<Native::ImageEntry*> frame_writebacks;
    void QueueWriteback(const Native::ImageEntry* entry);
    void WritebacksBefore(VAddr address, u64 size);
    void WritebacksBefore(DrawBindings& b);
    bool writebacks_due = false;
    std::vector<const Native::ImageEntry*> dump_entries; // NAT-116
    /// NAT-123: GPU-written images the dumped frame sampled, by texture key.
    std::vector<std::pair<std::string, const Native::ImageEntry*>> dump_textures;
    /// NAT-120: depth images by the address of their stencil plane (fills clear it there).
    std::unordered_map<VAddr, boost::container::small_vector<Native::ImageEntry*, 2>>
        stencil_owners;
    /// NAT-120: GPU work wrote guest memory (targets there get a pending rewrite).
    void GpuWrote(VAddr address, u64 size, const std::array<u32, 2>* pattern, bool exact);
    /// NAT-120: a target's pending rewrite, before it is used: a clear, or an upload.
    void ApplyRewrite(Native::ImageEntry& entry);
    /// NAT-132: fills the picture with a 32-bit pattern on the GPU (rows != 0: only those rows
    /// of the first mip and layer); false when it can't (compressed, multisampled).
    bool FillImage(Native::ImageEntry& entry, u32 lo, u32 hi, u32 rows, u32 first_layers = 1);
    /// NAT-134: a device buffer holding the pattern (lo, hi, lo, hi...), at least `bytes` long.
    const Native::Rhi::Buffer* PatternBuffer(u32 lo, u32 hi, u64 bytes);
    struct FillPattern {
        std::unique_ptr<Native::Rhi::Buffer> buffer;
        u64 last_use = 0;
    };
    void TrimFillPatterns(u64 current_key);
    std::unordered_map<u64, FillPattern> fill_patterns;
    u64 fill_pattern_clock = 0;
    /// NAT-120: GT Sport's fill shader (cs 0x8aa6e00c): its pattern and range, from the
    /// arguments it reads from memory.
    void ReadFillShader(const Shader::Info& cs, DrawBindings& b);
    /// NAT-104: the picture was written now (in GPU order).
    /// `at` != 0: the version is that clock value (NAT-148: a rewrite applied late).
    void Written(const Native::ImageEntry* entry, u64 at = 0);
    /// NAT-104: copies the newer pictures of the render targets a texture is made of into it.
    void RefreshSources(Native::ImageEntry& texture);

    void ResolveDraw(const AmdGpu::Regs& regs);
    bool MetaClearDispatch(const Shader::Info& cs);
    void Report();

    /// OPT-010: a draw's bindings worked out on the game job thread that recorded it, before
    /// the command processor reaches it: BindStages' lookups (buffers bound from regions or
    /// guest memory, textures from existing pictures and their views, samplers) with nothing
    /// done that must happen in GPU order (no picture or region made, nothing copied, uploaded
    /// or rewritten; upload memory is filled later). The recorder takes it by the draw packet's
    /// address and uses it when it was made for the same pipeline and user data and no
    /// picture, region or mirror over what it binds was made or dropped since (and no memory
    /// unmapped); then it does the ordered part (pending rewrites, copies in, upload memory).
    struct PreparedDraw {
        const void* packet = nullptr;
        u32 epoch = 0;
        const GraphicsPipeline* pipeline = nullptr;
        std::shared_ptr<const void> pipeline_keepalive;
        std::atomic<std::shared_ptr<void>> ahead_descriptor_lease;
        // Producer-only metadata, published by the existing prepared-draw shard.
        struct CpuProof {
            struct StageSchema { u64 identity{}; size_t count{}; size_t table_local{SIZE_MAX}; size_t table_upload{SIZE_MAX}; };
            std::array<StageSchema, Shader::MaxStageTypes - 1> stages{};
            const void* prepared_identity{}; const void* pipeline_identity{}; vk::PipelineLayout pipeline_layout{};
            const void* writes_data{}; size_t writes_size{}; const void* buffers_data{}; size_t buffers_size{}; const void* images_data{}; size_t images_size{};
            const void* vertex_identity{}; const void* vertex_buffers_data{}; size_t vertex_buffers_size{};
            bool descriptor_structure{},vertex_shape{},vertex_bindings{},vertex_pnext{};
            std::array<u64,2> image_seal_mask{}; u64 image_seal_infos{},image_seal_rows{},image_seal_epoch{}; const void* image_seal_owner{};
            std::array<const void*, Shader::Backend::MaxDescriptorSets> group_data{};
            std::array<size_t, Shader::Backend::MaxDescriptorSets> group_sizes{};
            size_t ordinary_writes{}, ordinary_buffers{}, ordinary_images{};
            u32 counts_push{}; bool counts_valid{};
            u32 push_index{}; const void* push_data{}; size_t push_size{}, push_buffer_first{}, push_buffer_count{};
            std::shared_ptr<void> push_snapshot;
        } cpu_proof;
        // Bounded producer action plan; capacity is retained when the draw is recycled.
        struct DescriptorAction {
            u32 upload_size{};
            u16 buffer_first{}, stage_local{};
            u8 range_hint{255}, region_hint{255};
            u8 stage_index{255};
            // 0: no matching upload, 1: unique matching upload, 2: use the original scan.
            u8 upload_match{2};
            bool is_buffer{};
        };
        static_assert(sizeof(DescriptorAction)==16);
        std::vector<DescriptorAction> descriptor_actions;
        // Factory-proven buffer prefix; any failed guard retains the original loop.
        bool buffer_prefix_sealed{}, buffer_prefix_clip{};
        const VideoCore::DirectMemory* buffer_prefix_direct{};
        u64 buffer_prefix_l1{}, buffer_prefix_zero{};
        boost::container::small_vector<std::pair<VAddr,VAddr>,4> buffer_prefix_ranges;

        std::array<u64, Shader::MaxStageTypes> stage_hashes{};
        u64 image_generation = 0;
        u64 buffer_seq = 0;
        u64 unmap_gen = 0;
        DrawBindings bindings;
        // Private heap packet metadata; no NativeRenderer layout change.
        std::vector<Native::Parallel::SampledViewProof<vk::ImageView>> sampled_view_proofs;
        std::shared_ptr<Native::Parallel::ProofProducerContext> proof_context;
        u64 proof_mode_state{};
        struct RegionUse {
            Region* region;
            VAddr address;
            u64 size;
            bool operator==(const RegionUse& other) const { return region == other.region && address == other.address && size == other.size; }
        };
        boost::container::small_vector<RegionUse, 4> regions;
        std::vector<std::pair<VAddr, VAddr>> image_ranges;
        std::vector<std::pair<VAddr, VAddr>> buffer_ranges;
        /// Upload memory (flattened user data, clip planes, D3 tables): bytes copied at commit
        /// into the list's upload memory, and the buffer info that binds them.
        struct Upload {
            u32 info = 0;
            u32 offset = 0;
            u32 size = 0;
            s32 flat_stage = -1; ///< >= 0: that stage's flattened user data, the recorder's
        };
        std::vector<u8> upload_bytes;
        boost::container::small_vector<Upload, 4> uploads;
        u64 upload_receipt_incarnation = 0;
        u32 null_images = 0;
        /// OPT-011: vertex inputs and buffers and the index buffer (not for quad lists), as
        /// ResolveVertexBuffers and ResolveIndices work them out; the V#s they came from (a
        /// hash), the mirrors bound (copied in at commit) and the ranges read in place (mirror
        /// candidates, noted at commit).
        bool native_vertex_receipt = false, native_index_receipt = false;
        bool has_vertices = false;
        std::unique_ptr<VertexIndexState> vertex_index;
        u64 vertex_hash = 0;
        bool indexed = false;
        u32 index_offset = 0;
        VAddr index_address = 0;
        boost::container::small_vector<Mirror*, 4> mirrors;
        boost::container::small_vector<std::pair<VAddr, u64>, 4> in_place;
        /// OPT-012: the stages as the job thread matched them (what the command thread's
        /// selection would copy): the recorder installs them when the command thread didn't
        /// select (HasPrepared).
        struct StageCopy {
            bool present = false;
            VAddr pgm_base = 0;
            u32 num_user_data = 0;
            std::array<u32, 16> user_data{};
            std::vector<u32> flattened;
        };
        std::array<StageCopy, Shader::MaxStageTypes> stage_copies;
        struct SamplerOrigin { size_t slot{}; vk::Sampler handle{}; };
        boost::container::static_vector<SamplerOrigin,Shader::NUM_SAMPLERS*(Shader::MaxStageTypes-1)> sampler_origins;
        u64 sampler_origins_epoch{};
        bool sampler_origins_valid{true};
        void Reset();
    };
    /// OPT-010: the guest ranges whose region or mirror was made or dropped, by sequence number
    /// (written and checked by the recorder; job threads read the sequence before looking up).
    class ChangeLog {
    public:
        void Note(VAddr begin, VAddr end);
        /// Whether none of the ranges changed after `since` (false when the log no longer
        /// reaches back that far).
        bool Untouched(u64 since, std::span<const std::pair<VAddr, VAddr>> ranges) const;
        u64 Seq() const {
            return seq.load(std::memory_order_acquire);
        }

    private:
        struct Entry {
            u64 seq = 0;
            VAddr begin = 0;
            VAddr end = 0;
        };
        std::array<Entry, 1024> entries{};
        std::atomic<u64> seq{0};
    };
    ChangeLog buffer_log;
    /// OPT-010: regions and mirrors are added and removed under it (exclusive); job threads
    /// look regions up under it (shared).
    std::shared_mutex buffer_map_mutex;
    struct PreparedShard {
        std::mutex mutex;
        std::unordered_map<const void*, PreparedDraw*> draws;
    };
    std::array<PreparedShard, 64> prepared_shards;
    PreparedShard& ShardFor(const void* packet) {
        return prepared_shards[(reinterpret_cast<u64>(packet) * 0x9E3779B97F4A7C15ULL) >> 58];
    }
    std::mutex prepared_pool_mutex;
    std::vector<std::unique_ptr<PreparedDraw>> prepared_all;
    std::vector<PreparedDraw*> prepared_free;
    PreparedDraw* NewPrepared();
    void FreePrepared(PreparedDraw* draw);
    /// The prepared draw for the packet, taken out of the table (none if there is none).
    PreparedDraw* TakePrepared(const void* packet);
    bool PrepareBuffers(const Shader::Info& stage, const AmdGpu::Regs& regs,
                        Shader::Backend::Bindings& binding, PreparedDraw& p);
    bool PrepareTextures(const Shader::Info& stage, const AmdGpu::Regs& regs,
                         Shader::Backend::Bindings& binding, PreparedDraw& p);
    bool PrepareVertices(PreparedDraw& p, const AmdGpu::Regs& regs,
                         const GraphicsPipeline& pipeline, const Shader::Info& vs,
                         bool is_indexed, u32 index_offset);
    /// A vertex or index range from a region, a mirror or guest memory read in place (none
    /// when it would need ordered work).
    std::optional<std::pair<vk::Buffer, u64>> JobVertexBuffer(VAddr address, u64 size,
                                                              PreparedDraw& p,
                                                              Mirror** mirror_out,
                                                              bool* native_receipt);
    bool CommitPreparedVertices(PreparedDraw& p, const GraphicsPipeline& pipeline,
                                const AmdGpu::Regs& regs, bool is_indexed, u32 index_offset,
                                bool quad_triangles, bool trusted);
    static u64 VertexHash(const GraphicsPipeline& pipeline, const Shader::Info& vs,
                          u32 step_rate_0, u32 step_rate_1);
    /// GuestBufferLookup's lookups without its side effects: none when it would make a region,
    /// gather or copy.
    std::optional<std::pair<vk::Buffer, u64>> JobGuestBuffer(VAddr address, u64 size, bool write,
                                                             PreparedDraw& p);
    /// Recorder: whether the prepared draw holds for the draw being recorded (its ordered part
    /// is then done).
    bool CommitPrepared(PreparedDraw& p, const GraphicsPipeline& pipeline,
                        const AmdGpu::Regs& regs, bool trusted);
    /// OPT-012: installs the prepared draw's stages for its pipeline (nullptr if they don't
    /// fit it).
    const GraphicsPipeline* InstallPrepared(PreparedDraw& p);
    static u64 StageHash(const Shader::Info& info);
    std::atomic<u64> shared_frame{0}; ///< OPT-010: `frame`, for job threads
    std::atomic<u32> recorder_epoch{0};
    struct PrepareStats {
        std::atomic<u64> made{0}, failed{0}, replaced{0}, stale{0}, ns{0};
    } prepare_stats;

    Rasterizer& r;
    std::unique_ptr<Native::Rhi::Device> device;
    std::shared_ptr<OneOffObserver> one_off_observer;
    std::unique_ptr<AsyncMirrorBuffer<Native::Rhi::Buffer>> mirror_preparation;
    std::unique_ptr<Native::Rhi::Fence> timeline;
    std::unique_ptr<Native::ImageRegistry> registry;
    std::unique_ptr<Native::Rhi::Buffer> gds;
    std::unique_ptr<Native::Rhi::Buffer> lds;
    std::unique_ptr<Native::Rhi::Image> composition_scratch;
    bool composition_scratch_initialized = false;

    std::recursive_mutex mutex; ///< v0: the recorder thread records; flips may come from others
    struct SnapshotUploadMemo;
    std::unique_ptr<SnapshotUploadMemo> snapshot_upload_memo; ///< guarded by mutex
    Stream stream;
    DrawBindings bindings;
    u64 frame = 0;
    u64 timing_tick = 0; ///< OPT-001
    u64 ui_draws = 0;
    u64 ui_resolves = 0;
    u64 smooth_ghost_draws = 0;
    /// NAT-142: guest memory the GPU reads or writes in place (64 KB pages, every 8th frame):
    /// vertex buffers, indices, read-only storage through the page table, other storage
    /// buffers read, storage buffers written; NAT-150: then what is bound from video memory
    /// instead (mirrors, regions).
    struct InPlace {
        std::array<PageCoverage, 7> pages;
        std::array<u64, 7> total{};
        u64 frames = 0;
        void Note(u32 kind, VAddr address, u64 size) {
#if !defined(NDEBUG)
            pages[kind].Add(address, size);
#endif
        }
    } in_place;
    bool InPlaceSampled() const {
#if !defined(NDEBUG)
        // PERF-GT7-001: collecting per-binding page coverage is opt-in.
        static const bool on = [] {
            const char* env = std::getenv("RYGT_NATIVE_INPLACE_STATS");
            return env && env[0] == '1';
        }();
        return on && (frame & 7) == 0;
#else
        return false;
#endif
    }
    /// NAT-150: the counter for a binding of that kind, by what GuestBuffer bound.
    u32 BoundKind(u32 kind) const {
        return last_bound == Bound::Mirror ? 5 : (last_bound == Bound::Region ? 6 : kind);
    }

    std::mutex chunk_mutex;
    std::vector<Chunk> free_chunks;
    size_t free_chunks_low = 0; // minimum completed/free count since the last trim
    Native::RetiredSnapshotPool<Native::Rhi::Buffer> snapshot_pool;
    Native::RetiredWritebackPlans retired_writeback_plans;
    PipelineOwnerRetirement retired_pipeline_owners;
    std::deque<Chunk> retiring_chunks;
    std::vector<vk::DescriptorPool> free_pools;
    std::deque<std::pair<u64, vk::DescriptorPool>> retiring_pools;

    std::unordered_map<u64, vk::Sampler> sampler_lookup;
    vk::Sampler last_sampler{};
    std::mutex sampler_mutex;
    std::unordered_map<u64, std::unique_ptr<VideoCore::Sampler>> samplers;

    std::mutex views_mutex;
    std::unordered_map<const Native::ImageEntry*, std::vector<ViewRecord>> views;
    /// NAT-108: views of evicted images, destroyed once the GPU passed the tick.
    std::deque<std::pair<u64, vk::UniqueImageView>> retired_views;
    void EvictImages();
    struct ReportBase {
        u64 uploads = 0, upload_bytes = 0, evicted = 0;
        std::array<u64, 3> stale{};
    } report_base;

    /// NAT-104: the write clock, and which version of its sources each subresource of a
    /// texture holds (by mip << 16 | layer).
    u64 write_clock = 0;
    std::unordered_map<const Native::ImageEntry*, std::unordered_map<u32, u64>> held_versions;

    /// NAT-105: the pages native textures watch (each watched once in the page manager, which
    /// counts at most 255 watchers a page; many textures share pages).
    PageWatchTracker page_watches;
    // Serializes fill-journal publication with page-unwatch and CPU fault invalidation.
    std::mutex fill_watch_mutex;
    std::vector<std::pair<VAddr, VAddr>> fill_watched_ranges;

    /// CMASK / HTILE addresses the game cleared: the next pass into their target clears it.
    std::unordered_set<VAddr> cleared_meta;
    /// Color targets by their CMASK address, depth targets by their HTILE address (so a clear
    /// of the metadata can be told from a clear of a picture).
    std::unordered_set<VAddr> known_meta;

#ifndef NDEBUG
    u64 stats_reset_generation{};
#endif
    struct Stats {
#ifndef NDEBUG
        u64 snapshot_memo_hits = 0, snapshot_memo_misses = 0, snapshot_upload_bytes_saved = 0;
        u64 snapshot_direct_attempts = 0, snapshot_direct_hits = 0, snapshot_direct_faults = 0, snapshot_direct_bytes = 0;
#endif
        u64 rewrite_clears = 0, rewrite_uploads = 0, fill_shaders = 0; ///< NAT-120
        u64 rewrite_partial = 0; ///< NAT-124
        u64 pass_barriers = 0, passes_unblocked = 0; ///< NAT-140
        u64 region_refresh_pages = 0, region_refresh_ns = 0; ///< NAT-145
        u64 region_partial_pages = 0, region_partial_bytes = 0; ///< FIX-173
        u64 mirror_untrapped = 0;                               ///< FIX-176
        u64 mirror_checks = 0, mirror_check_differ = 0;         ///< FIX-179
        u64 reach_checks = 0, reach_past_sharp = 0, reach_past_mirror = 0; ///< FIX-181
        u64 guest_buffer_calls = 0, guest_buffer_ns = 0;     ///< NAT-145
        u64 rewrite_fills = 0;   ///< NAT-132
        u64 timed_draws = 0;     ///< OPT-001
        u64 targets_reused = 0;
        u64 vertex_near = 0;     ///< OPT-004
        u64 vertex_skipped = 0;  ///< OPT-005
        /// OPT-010: draws bound from a prepared draw, without one, and prepared draws not used:
        /// another pipeline, other user data, memory unmapped, pictures or buffers changed.
        u64 flat_upload_hits = 0, flat_upload_bytes_saved = 0, flat_upload_misses = 0;
        u64 flat_upload_bytes_hashed = 0, flat_upload_reference_bytes = 0, flat_upload_bytes_uploaded = 0;
        u64 descriptor_allocate_calls = 0, descriptor_reserve_hits = 0;
        u64 prepared_used = 0, prepared_missing = 0;
        std::array<u64, 5> prepared_rejected{};
        u64 prepared_vertices = 0, prepared_vertices_rejected = 0; ///< OPT-011
        u64 prepared_installed = 0; ///< OPT-012: pipelines from a job thread's selection
        /// OPT-011: the record phase's parts: barrier choice, pass begin, dynamic state, vertex
        /// and index binds, pipeline bind, descriptors and push constants, the draw, write notes.
        std::array<u64, 8> record_ns{};
        u64 descriptor_reuses = 0, descriptor_allocations = 0;
        u64 texture_memo_hits = 0, texture_lookups = 0; ///< OPT-002
        u64 buffer_memo_hits = 0;                        ///< OPT-003
        u64 mirror_binds = 0, mirrors_made = 0, mirror_made_bytes = 0, mirrors_dropped = 0,
            mirror_cross_address_reuses = 0,
            mirrors_evicted = 0, mirror_over_budget = 0, mirror_refresh_pages = 0; ///< NAT-150
        u64 draws = 0, dispatches = 0, skipped = 0, no_pipeline = 0, passes = 0, barriers = 0,
            splices = 0, copied_reads = 0, copied_bytes = 0, unbound = 0, null_images = 0,
            fills = 0, copies = 0, meta_clears = 0, inline_writes = 0, source_copies = 0,
            draw_ns = 0, dispatch_ns = 0, gathers = 0, gathered_bytes = 0, writebacks = 0,
            region_binds = 0, regions_made = 0, region_refreshes = 0;
        /// Images bound empty: no address or format, not mapped for the GPU, format the
        /// device lacks, bad geometry, outside guest memory, FIX-081, no image made.
        std::array<u64, 7> null_reasons{};
        /// NAT-110: time in the draw's phases: pipeline, targets, buffers, textures, sources,
        /// vertices, prepare (inits and uploads), record.
        std::array<u64, 8> phase_ns{};
        std::atomic<u64> cpu_writes{0}; // NAT-105 (faults come from any thread)
    } stats;
    // Accessed only by paths holding the existing renderer mutex, including OnUnmap.
    struct MirrorDiagnostic {
        struct Example {u32 reason{};u64 requested_begin{},requested_end{},expanded_begin{},expanded_end{},generation{},granule{},age{};};
        std::array<u64,21> counts{};
        std::array<Example,16> examples{};
        u32 example_count{};
        std::array<u64, 3> source_outcomes{}; // missing, busy, invalid probe
        u64 warm_attempts{}, warm_queued{};
    } mirror_diagnostic;
#if !defined(NDEBUG)
    std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();
#else
    std::chrono::steady_clock::time_point last_report{};
#endif
};

} // namespace Vulkan
