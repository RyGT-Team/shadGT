// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <vector>

#include "common/types.h"
#include "video_core/buffer_cache/imported_page_coverage.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {

struct Buffer;
class ImportCleanupWorker;
class ImportedViewLease;

/// D1 (direct memory model, SHADGT_DIRECT=1): the GPU reads guest memory itself.
///
/// Each guest view (a run of one memory section, as Ryty maps it) gets a writable alias view
/// (GuestArenaMapAlias) that is imported into Vulkan (VK_EXT_external_memory_host). The game's
/// own view keeps whatever protection the renderer's write tracking or the game sets; the GPU
/// goes through the alias, which never changes. A two-level page table on the GPU maps each
/// 16 KiB guest page to the device address of its bytes:
///   L1[address >> 25]           -> device address of an L2 table (0: nothing mapped there)
///   L2[(address >> 14) & 2047]  -> device address of the page (0: not imported)
/// Shaders read buffers through it, so no CPU work is needed to make guest bytes visible.
class DirectMemory {
public:
    static constexpr u32 PageBits = 14;
    static constexpr u64 VolatileWords = (u64{1} << (40 - PageBits)) / 64;
    static constexpr u64 PageSize = u64{1} << PageBits;
    static constexpr u32 L2Bits = 11; // 2048 pages = 32 MiB per L1 entry
    static constexpr u32 L1Bits = 40 - PageBits - L2Bits;
    static constexpr u64 L1Entries = u64{1} << L1Bits;
    static constexpr u64 L2Entries = u64{1} << L2Bits;
    static constexpr u32 MaxL2Tables = 4096; // 128 GiB of guest address space

    explicit DirectMemory(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
    ~DirectMemory();
    u64 ImportedHostBytes() const { return imported_bytes.load(std::memory_order_relaxed); }

    /// Whether the model is on (SHADGT_DIRECT=1 and the driver can import host memory).
    static bool Enabled();

    /// Imports the views covering [address, address + size) not imported yet. True when every
    /// page of the range is now mapped for the GPU.
    bool EnsureImported(VAddr address, u64 size);
    /// FIX-205 (shadgt7's interface): no-ops here. Dropped views are leases that the native
    /// renderer holds through HoldNativeReleases / ReleaseHeldAfterSplice instead.
    static void HoldReleasesForSplice();
    static void ReleaseHeldAfter(u64 tick);

    /// Best-effort worker warm-up: never waits for the view mutex, imports at most
    /// eight small views per visit, and does not run pending-release callbacks.
    void WarmImported(VAddr address, u64 size);

    /// D6: the imported buffer holding the whole range and the range's offset in it, when one
    /// view holds it (call EnsureImported first).
    std::optional<std::pair<const Buffer*, u64>> FindBuffer(VAddr address, u64 size) const;

    /// PERF-GTS-472 (RYGT_DIRECT_MIN_BYTES, default 65536, 0 off): read-only buffers no larger
    /// than this when their shader is translated get no direct-read path and are bound as usual.
    static u32 DirectMinBytes();

    /// NAT-107: the imported views holding [address, address + size) in order:
    /// fn(view buffer, offset in it, offset in the range, bytes). False (after calling fn for
    /// the parts before it) when part of the range is not imported.
    bool ForEachPart(VAddr address, u64 size,
                     const std::function<void(const Buffer&, u64, u64, u64)>& fn) const;

    struct LeasedPart {
        vk::Buffer buffer;
        u64 view_offset, range_offset, size, view_size;
        std::shared_ptr<void> lease;
    };
    // Complete coverage and alignment are checked before returning any parts. Each lease keeps
    // the VkBuffer, VkDeviceMemory and alias alive even after the view is unmapped.
    std::optional<std::vector<LeasedPart>> PlanLeasedParts(VAddr address, u64 size,
                                                           u64 alignment, u64 size_alignment) const;
    // Transfer stream leases only after its actual submission tick is known.
    bool RetireLeasesAfterSplice(std::vector<std::shared_ptr<void>>& leases, u64 tick);
    void HoldNativeReleases();
    bool ReleaseHeldAfterSplice(u64 tick, bool stop_holding = false);
    void ReleaseHeldAfterGpuIdle(); // caller has waited for all prior submissions

    // Optional copies: never wait for an importer. Validate the entire range
    // before acquiring the destination or recording any copy. Callbacks must
    // not reenter DirectMemory; views stay protected through all copies.
    enum class PartOutcome { Invalid, Busy, MissingCoverage, Covered };
    bool TryForEachPart(VAddr address, u64 size, const std::function<bool()>& ready,
                        const std::function<void(const Buffer&, u64, u64, u64)>& fn,
                        PartOutcome* outcome = nullptr) const;

    /// Whether every page of the range is mapped for the GPU (no import attempted).
    bool Covered(VAddr address, u64 size) const;

    /// Ryty unmaps guest memory (before it does): the pages leave the page table; the aliases are
    /// released once the GPU has finished the work recorded so far.
    void OnUnmap(VAddr address, u64 size);

    /// FIX-GT7-077: Ryty mapped [address, address + size) (it reports after it did). Whether views
    /// of the memory mapped there before are still in the page table: a view imported between
    /// Ryty's unmap report and its unmap is an alias of the old pages, which the game then gives
    /// to other data. Bases that failed to import there may import again.
    bool ViewsOver(VAddr address, u64 size);

    /// D4: the command processor rewrites this range while queued draws may still read it (CE
    /// dumps, WriteData): its buffers keep the copy path, which snapshots at bind time.
    void MarkVolatile(VAddr address, u64 size);
    /// Whether any page of the range was marked volatile.
    bool IsVolatile(VAddr address, u64 size) const;

    /// Device address of the L1 table, for the shaders.
    vk::DeviceAddress L1Address() const;
    /// A page of zeros: where unmapped addresses resolve.
    vk::DeviceAddress ZeroPageAddress() const;

    struct Stats {
        u64 views;
        u64 bytes;
        u64 failed;
    };
    Stats GetStats() const;

private:
    struct View {
        VAddr begin;
        VAddr end;
        void* alias;
        VkDeviceMemory memory;
        VkBuffer buffer;
        vk::DeviceAddress device_address;
        std::shared_ptr<ImportedViewLease> lease;
        std::shared_ptr<Buffer> wrapper; // D6: owns `buffer`; bound as vertex/index buffer
    };
    // PERF-GT7-022: at most two allocation-relative pieces per import,
    // trimmed around already imported views; preserves non-overlap.
    /// Imports the view holding `address`: PERF-GT7-022: the 256 MiB piece of its allocation
    /// that holds it, or up to `want_end` (512 MiB at most) when the pieces up to there are not
    /// imported yet, so a range asked for in one go stays in one view.
    bool ImportView(VAddr address, VAddr want_end);
    void MapPages(VAddr begin, VAddr end, vk::DeviceAddress device_address);
    void UnmapPages(VAddr begin, VAddr end);
    u64* L2For(VAddr address, bool create);
    void Release(View& view);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    mutable std::mutex mutex;
    ImportedPageCoverage<> imported_coverage;
    std::map<VAddr, View> views; // by begin
    bool hold_native_releases{};
    std::vector<std::shared_ptr<void>> held_native_releases;
    std::map<VAddr, VAddr> failed; // failed view base -> committed end; invalidated on unmap
    u64 failed_generation{};
    std::unique_ptr<Buffer> l1;
    std::unique_ptr<Buffer> l2_pool;
    std::unique_ptr<Buffer> zero_page;
    std::unique_ptr<std::atomic<u64>[]> volatile_bits; // D4: one bit per 16 KiB page, 2^40 bytes
    u32 l2_used{};
    std::vector<u32> l2_index; // L1 entry -> L2 table + 1 (0: none)
    std::atomic<u64> imported_bytes{};
    std::atomic<u64> failed_count{};
    std::atomic<u64> volatile_pages{};
    std::shared_ptr<ImportCleanupWorker> import_cleanup;
};

} // namespace VideoCore
