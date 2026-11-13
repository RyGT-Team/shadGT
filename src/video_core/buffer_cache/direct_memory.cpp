// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <stop_token>
#include "ryty_gpu/native/descriptor_dead_handles.h"
#include <thread>
#include <condition_variable>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include "ryty_gpu/native/memory_range_runs.h"
#include "ryty_gpu/native/epoch_lookup_memo.h"
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

#include "common/thread.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/direct_memory.h"
#include "video_core/buffer_cache/defer_import_releases.h"
#include "video_core/buffer_cache/import_cleanup_worker.h"
#include "video_core/buffer_cache/imported_view_lease.h"
#include "video_core/buffer_cache/imported_part_plan.h"
#include "ryty_gpu/native/slow_call_trace.h"
#include "ryty_gpu/native/direct_coherence_registry.h"
#include "ryty_gpu/native/parallel_direct_capability.h"
#include "ryty_gpu/native/snapshot_backing_protocol.h"
#include "video_core/buffer_cache/proto_import.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace VideoCore {

namespace {
// Every view insertion/removal and owner lifetime change invalidates all local
// shortcuts. The view mutex still protects each lookup and returned wrapper.
std::atomic<u64> imported_view_epoch{1};
Native::Parallel::DirectCoherenceRegistry<> direct_coherence;
bool TrackParallelDirect() noexcept {
    static const bool active=[] { const char* p=std::getenv("RYGT_PARALLEL_D2"); return p && p[0]=='1' && p[1]=='\0'; }();
    return active;
}

using MapAliasFn = void* (*)(std::uintptr_t, std::size_t);
using UnmapAliasFn = void (*)(void*);

MapAliasFn MapAlias() {
#ifdef _WIN32
    const auto& protocol = Vulkan::SnapshotLease::BackingProtocol::Get();
    // Alias bridge capability is distinct from snapshot admission. A cold
    // driver prerequisite must not permanently select the legacy alias that
    // poisons every future range. All actual reuse still checks the full gate.
    if (protocol.AliasBridgeAvailable())
        return reinterpret_cast<MapAliasFn>(protocol.renderer_alias);
    static const auto fn = [] {
        HMODULE libc = GetModuleHandleA("libc.prx");
        return libc ? reinterpret_cast<MapAliasFn>(GetProcAddress(libc, "TXQU8CpoEtI")) : nullptr;
    }();
    return fn;
#else
    return nullptr;
#endif
}

UnmapAliasFn UnmapAlias() {
#ifdef _WIN32
    static const auto fn = [] {
        HMODULE libc = GetModuleHandleA("libc.prx");
        return libc ? reinterpret_cast<UnmapAliasFn>(GetProcAddress(libc, "674-w6lwWXI"))
                    : nullptr;
    }();
    return fn;
#else
    return nullptr;
#endif
}

} // namespace

bool ParallelDirectViewsCoherent(const DirectMemory* owner, std::span<const std::pair<std::uint64_t,std::uint64_t>> ranges) noexcept {
    return ranges.empty() || (TrackParallelDirect() && direct_coherence.CoversAll(owner,ranges));
}

bool ParallelDirectViewCoherent(const DirectMemory* owner, std::uint64_t base, std::uint64_t size) noexcept {
    return TrackParallelDirect() && direct_coherence.Covers(owner,base,size);
}

DirectMemory::DirectMemory(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    imported_view_epoch.fetch_add(1, std::memory_order_acq_rel);
    l1 = std::make_unique<Buffer>(instance, 0, L1Entries * sizeof(u64), MemoryType::Stream,
                                  "D1 page table L1");
    l2_pool = std::make_unique<Buffer>(instance, 0, MaxL2Tables * L2Entries * sizeof(u64),
                                       MemoryType::Stream, "D1 page table L2");
    zero_page = std::make_unique<Buffer>(instance, 0, 2 * PageSize, MemoryType::Stream,
                                         "D1 zero page");
    if (TrackParallelDirect()) direct_coherence.Register(this,
        l1->is_coherent && l2_pool->is_coherent && zero_page->is_coherent);
    std::memset(zero_page->mapped_data.data(), 0, zero_page->mapped_data.size());
    std::memset(l1->mapped_data.data(), 0, l1->mapped_data.size());
    l2_index.assign(L1Entries, 0);
    volatile_bits = std::make_unique<std::atomic<u64>[]>(VolatileWords);
    import_cleanup = std::make_shared<ImportCleanupWorker>([] {
        Common::SetCurrentThreadName("shadGT:ImportCleanup");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
#ifdef _WIN32
        SetThreadPriorityBoost(GetCurrentThread(), TRUE);
#endif
    });
    LOG_WARNING(Render_Vulkan, "D1: direct memory on (page table L1 at {:#x})",
                l1->BufferDeviceAddress());
}

DirectMemory::~DirectMemory() {
    if (TrackParallelDirect()) direct_coherence.Erase(this);
    imported_view_epoch.fetch_add(1, std::memory_order_acq_rel);
    // Active mapped views can still be referenced by the final non-native submission. Their
    // lease deleters free Vulkan memory when the map is destroyed immediately after this body.
    if (!views.empty() || !held_native_releases.empty()) scheduler.Finish();
}

bool DirectMemory::Enabled() {
    // D11: the direct renderer is the renderer (GPU.direct_renderer from older launchers is
    // ignored). SHADGT_DIRECT=0 still turns it off, for comparisons.
    static const bool enabled = [] {
        if (const char* env = std::getenv("SHADGT_DIRECT"); env && *env) {
            return env[0] == '1';
        }
        return true;
    }();
    return enabled;
}

void DirectMemory::MarkVolatile(VAddr address, u64 size) {
    const u64 first = address >> PageBits;
    const u64 last = (address + std::max<u64>(size, 1) - 1) >> PageBits;
    for (u64 page = first; page <= last && (page >> 6) < VolatileWords; ++page) {
        const u64 bit = u64{1} << (page & 63);
        if (!(volatile_bits[page >> 6].fetch_or(bit, std::memory_order_relaxed) & bit)) {
            if (const u64 n = ++volatile_pages; n <= 20 || n % 100 == 0) {
                LOG_WARNING(Render_Vulkan, "D4: page {:#x} is volatile ({} pages)",
                            page << PageBits, n);
            }
        }
    }
}

bool DirectMemory::IsVolatile(VAddr address, u64 size) const {
    const u64 first = address >> PageBits;
    const u64 last = (address + std::max<u64>(size, 1) - 1) >> PageBits;
    if ((last >> 6) >= VolatileWords) {
        return true;
    }
    // A word (64 pages, 1 MiB) at a time.
    for (u64 word = first >> 6; word <= (last >> 6); ++word) {
        u64 mask = ~u64{0};
        if (word == (first >> 6)) {
            mask &= ~u64{0} << (first & 63);
        }
        if (word == (last >> 6)) {
            mask &= ~u64{0} >> (63 - (last & 63));
        }
        if (volatile_bits[word].load(std::memory_order_relaxed) & mask) {
            return true;
        }
    }
    return false;
}

vk::DeviceAddress DirectMemory::ZeroPageAddress() const {
    return zero_page->BufferDeviceAddress();
}

vk::DeviceAddress DirectMemory::L1Address() const {
    return l1->BufferDeviceAddress();
}

u64* DirectMemory::L2For(VAddr address, bool create) {
    const u64 index = address >> (PageBits + L2Bits);
    if (index >= L1Entries) {
        return nullptr;
    }
    if (l2_index[index] == 0) {
        if (!create || l2_used >= MaxL2Tables) {
            return nullptr;
        }
        const u32 table = l2_used++;
        u64* entries = reinterpret_cast<u64*>(l2_pool->mapped_data.data()) + table * L2Entries;
        std::memset(entries, 0, L2Entries * sizeof(u64));
        const u64 table_address = l2_pool->BufferDeviceAddress() + table * L2Entries * sizeof(u64);
        reinterpret_cast<u64*>(l1->mapped_data.data())[index] = table_address;
        l2_index[index] = table + 1;
    }
    return reinterpret_cast<u64*>(l2_pool->mapped_data.data()) +
           (l2_index[index] - 1) * L2Entries;
}

void DirectMemory::MapPages(VAddr begin, VAddr end, vk::DeviceAddress device_address) {
    Native::VisitPageTableRuns<PageBits, L2Bits>(begin, end,
      [&](u64 address, u64 first, u64 count) {
        if (u64* l2 = L2For(address, true)) {
            for (u64 i = 0; i < count; ++i)
                l2[first + i] = device_address + (address - begin) + i * PageSize;
        }
      });
}

void DirectMemory::UnmapPages(VAddr begin, VAddr end) {
    Native::VisitPageTableRuns<PageBits, L2Bits>(begin, end,
      [&](u64 address, u64 first, u64 count) {
        if (u64* l2 = L2For(address, false)) {
            std::fill_n(l2 + first, count, u64{0});
        }
      });
}

bool DirectMemory::Covered(VAddr address, u64 size) const {
    return imported_coverage.Covers(address, size);
}

std::optional<std::pair<const Buffer*, u64>> DirectMemory::FindBuffer(VAddr address,
                                                                     u64 size) const {
    // PERF-GT7-127 (RYGT_DIRECT_FIND_RECENT=0 off): in a 20-car race the recorder asked for a
    // view ~10,000 times a frame, each time under the lock and down the map (2.5% of its busy
    // time in the map walk alone), almost always for one of a few dozen views. The thread's
    // own memo of the views it found is checked before the lock: it is this thread's alone,
    // and a view only goes when imported_view_epoch moves on.
    static const bool recent_on = [] {
        const char* env = std::getenv("RYGT_DIRECT_FIND_RECENT");
        return !(env && env[0] == '0');
    }();
    static thread_local Native::EpochRangeMemo<const Buffer*> memo;
    if (recent_on) {
        const auto epoch = imported_view_epoch.load(std::memory_order_acquire);
        if (const auto found = memo.Find(this, epoch, address, size))
            return std::make_pair(found->value, found->offset);
    }
    std::scoped_lock lk{mutex};
    const auto epoch = imported_view_epoch.load(std::memory_order_acquire);
    if (const auto found = memo.Find(this, epoch, address, size))
        return std::make_pair(found->value, found->offset);
    auto it = views.upper_bound(address);
    if (it == views.begin()) {
        return std::nullopt;
    }
    const View& view = std::prev(it)->second;
    if (address < view.begin || address > view.end || size > view.end - address || !view.wrapper) {
        return std::nullopt;
    }
    memo.Store(this, epoch, view.begin, view.end, view.wrapper.get());
    return std::make_pair(static_cast<const Buffer*>(view.wrapper.get()), address - view.begin);
}

u32 DirectMemory::DirectMinBytes() {
    static const u32 bytes = [] {
        const char* env = std::getenv("RYGT_DIRECT_MIN_BYTES");
        return env && *env ? static_cast<u32>(std::strtoul(env, nullptr, 0)) : 65536u;
    }();
    return bytes;
}

namespace {
// PERF-GT7-022 (Will Scarlett): import a bounded piece when first used rather
// than pinning a whole multi-GiB allocation in one driver call. Zero restores
// the original whole-allocation policy. Invalid/overflowing input uses 256 MiB.
u64 ViewPiece() {
    static const u64 piece = [] {
        const char* env = std::getenv("RYGT_D1_VIEW_MB");
        u64 mb = 256;
        if (env && *env) {
            char* end = nullptr;
            errno = 0;
            const u64 parsed = std::strtoull(env, &end, 10);
            if (*env != '-' && end != env && *end == '\0' && errno != ERANGE && parsed <= 4096) {
                mb = parsed;
            }
        }
        return mb == 0 ? u64{1} << 32 : mb << 20;
    }();
    return piece;
}
} // namespace

bool DirectMemory::ImportView(VAddr address, VAddr want_end) {
    Native::SlowCallTrace<8> trace{Native::trace_guest_hash_calls, "import-guest-view",
        {"guest-view", "map-alias", "host-properties", "allocate-memory", "create-bind-buffer",
         "device-address", "map-pages", "store-view"}};
    trace.Next(0, address, 0);
#ifdef _WIN32
    const VAddr allocation = GuestViewBase(address);
    if (allocation == 0) {
        return false;
    }
    const u64 piece = ViewPiece();
    VAddr base = allocation + (address - allocation) / piece * piece;
    if (auto next = views.upper_bound(address); next != views.begin()) {
        base = std::max(base, std::prev(next)->second.end);
    }
    if (failed.contains(base)) {
        return false;
    }
    const VAddr pieces_end = want_end > base + piece
        ? std::min(base + 2 * piece,
                   allocation + (want_end - 1 - allocation) / piece * piece + piece)
        : base + piece;
    VAddr end = std::min(GuestViewEnd(base, pieces_end - base), pieces_end);
    if (const auto next = views.upper_bound(base); next != views.end()) {
        end = std::min(end, next->second.begin);
    }
    const auto fail = [&](const char* why) {
        // An unmap/remap can leave the allocation temporarily uncommitted or split
        // before the requested page. That is not a permanent import failure.
        if (end > address && (base & (PageSize - 1)) == 0 &&
            ((end - base) & (PageSize - 1)) == 0) {
            failed.emplace(base, end);
        }
        if (++failed_count <= 40) {
            LOG_WARNING(Render_Vulkan, "D1: view {:#x}+{:#x} not imported: {}", base, end - base,
                        why);
        }
        return false;
    };
    if (end <= base || end > ImportedPageCoverage<>::Limit ||
        (base & (PageSize - 1)) != 0 || ((end - base) & (PageSize - 1)) != 0) {
        return fail("not whole pages");
    }
    if (end <= address) {
        return fail("the address is past the piece"); // PERF-GT7-022: not expected
    }
    const auto map_alias = MapAlias();
    if (!map_alias) {
        return fail("no GuestArenaMapAlias");
    }
    trace.Next(1, base, end - base);
    void* alias = map_alias(base, end - base);
    if (!alias) {
        return fail("no alias (not shared memory)");
    }
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    const VkDevice device = instance.GetDevice();
    VkMemoryHostPointerPropertiesEXT ptr_props{
        VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    trace.Next(2);
    if (d.vkGetMemoryHostPointerPropertiesEXT(device,
                                              VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                              alias, &ptr_props) != VK_SUCCESS ||
        ptr_props.memoryTypeBits == 0) {
        UnmapAlias()(alias);
        return fail("no memory type for the alias");
    }
    VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr,
                                         VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            &flags_info,
                                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            alias};
    u32 memory_type_index = static_cast<u32>(std::countr_zero(ptr_props.memoryTypeBits));
    // FIX-188: Intel Arc (#65) imports into cached host memory (uncached stalls the CPU).
    const auto& mem_props = instance.GetMemoryProperties();
    for (u32 i = 0; instance.IsIntel() && i < mem_props.memoryTypeCount; ++i) {
        if ((ptr_props.memoryTypeBits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eHostCached)) {
            memory_type_index = i;
            break;
        }
    }
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import, end - base,
                              memory_type_index};
    VkDeviceMemory memory{};
    trace.Next(3);
    if (const VkResult r = d.vkAllocateMemory(device, &info, nullptr, &memory); r != VK_SUCCESS) {
        UnmapAlias()(alias);
        return fail("import refused");
    }
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                              nullptr,
                                              VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
    buffer_info.size = end - base;
    buffer_info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT; // NAT-100: dispatch arguments
    VkBuffer buffer{};
    trace.Next(4);
    if (d.vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS ||
        d.vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        if (buffer) {
            d.vkDestroyBuffer(device, buffer, nullptr);
        }
        d.vkFreeMemory(device, memory, nullptr);
        UnmapAlias()(alias);
        return fail("buffer over the import failed");
    }
    VkBufferDeviceAddressInfo address_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr,
                                           buffer};
    trace.Next(5);
    const vk::DeviceAddress device_address = d.vkGetBufferDeviceAddress(device, &address_info);
    if (!device_address) {
        d.vkDestroyBuffer(device, buffer, nullptr);
        d.vkFreeMemory(device, memory, nullptr);
        UnmapAlias()(alias);
        return fail("no device address");
    }
    trace.Next(6);
    const auto vk_device = instance.GetDevice();
    const auto unmap_alias = UnmapAlias();
    std::shared_ptr<Buffer> wrapper;
    std::shared_ptr<ImportedViewLease> lease;
    try {
        wrapper = std::make_shared<Buffer>(instance, base, end - base, vk::Buffer{buffer});
        lease = std::make_shared<ImportedViewLease>(
            [vk_device, unmap_alias, memory, alias, owned = wrapper]() mutable {
                owned.reset();
                VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeMemory(vk_device, memory, nullptr);
                unmap_alias(alias);
            });
        const auto [it, inserted] = views.emplace(
            base, View{base, end, alias, memory, buffer, device_address, lease, wrapper});
        if (!inserted) throw std::bad_alloc{};
    } catch (...) {
        const bool wrapper_owns_buffer = bool(wrapper);
        const bool lease_owns_memory = bool(lease);
        wrapper.reset();
        lease.reset();
        if (!wrapper_owns_buffer) {
            Native::DeadHandles::Note(buffer); // PERF-GTS-375
            d.vkDestroyBuffer(device, buffer, nullptr);
        }
        if (!lease_owns_memory) {
            d.vkFreeMemory(device, memory, nullptr);
            unmap_alias(alias);
        }
        return fail("could not retain imported buffer");
    }
    MapPages(base, end, device_address);
    trace.Next(7);
    if (TrackParallelDirect()) direct_coherence.Add(this,base,end,
        bool(mem_props.memoryTypes[memory_type_index].propertyFlags & vk::MemoryPropertyFlagBits::eHostCoherent));
    imported_view_epoch.fetch_add(1, std::memory_order_acq_rel);
    imported_bytes += end - base;
    imported_coverage.Publish(base, end); // publish only the completed view and page table
    if (views.size() <= 20 || views.size() % 100 == 0) {
        LOG_WARNING(Render_Vulkan, "D1: imported view {:#x}+{:#x} ({} views, {} MiB)", base,
                    end - base, views.size(), imported_bytes.load() >> 20);
    }
    return true;
#else
    return false;
#endif
}

bool DirectMemory::ForEachPart(VAddr address, u64 size,
                               const std::function<void(const Buffer&, u64, u64, u64)>& fn) const {
    std::scoped_lock lk{mutex};
    const VAddr end = address + size;
    for (VAddr cursor = address; cursor < end;) {
        auto it = views.upper_bound(cursor);
        if (it == views.begin()) {
            return false;
        }
        const View& view = std::prev(it)->second;
        if (cursor < view.begin || cursor >= view.end || !view.wrapper) {
            return false;
        }
        const u64 bytes = std::min(end, view.end) - cursor;
        fn(*view.wrapper, cursor - view.begin, cursor - address, bytes);
        cursor += bytes;
    }
    return true;
}

std::optional<std::vector<DirectMemory::LeasedPart>> DirectMemory::PlanLeasedParts(
    VAddr address, u64 size, u64 alignment, u64 size_alignment) const {
    std::scoped_lock lock{mutex};
    return PlanImportedParts<LeasedPart>(views, address, size, alignment, size_alignment,
        [](const View& view, u64 offset, u64 range_offset, u64 bytes) {
            return LeasedPart{view.wrapper->Handle(), offset, range_offset, bytes,
                              view.wrapper->SizeBytes(), view.lease};
        });
}

bool DirectMemory::RetireLeasesAfterSplice(std::vector<std::shared_ptr<void>>& leases, u64 tick) {
    if (!leases.empty() && tick == 0) return false;
    auto cleanup = import_cleanup;
    return RetireImportedLeases(leases,
        [&](auto callback) { scheduler.DeferPriorityOperationAt(tick, std::move(callback)); },
        [cleanup](auto held) {
            try { cleanup->Enqueue([held] { held->clear(); }); }
            catch (...) { held->clear(); } // GPU is already complete here.
        });
}

void DirectMemory::HoldNativeReleases() {
    std::scoped_lock lock{mutex};
    hold_native_releases = true;
}

bool DirectMemory::ReleaseHeldAfterSplice(u64 tick, bool stop_holding) {
    std::scoped_lock lock{mutex};
    const bool queued = RetireLeasesAfterSplice(held_native_releases, tick);
    if (queued && stop_holding) hold_native_releases = false;
    return queued;
}

void DirectMemory::ReleaseHeldAfterGpuIdle() {
    std::vector<std::shared_ptr<void>> held;
    {
        std::scoped_lock lock{mutex};
        hold_native_releases = false;
        held.swap(held_native_releases);
    }
    held.clear(); // GPU is idle; no view lock is held during driver destruction.
}

bool DirectMemory::TryForEachPart(
    VAddr address, u64 size, const std::function<bool()>& ready,
    const std::function<void(const Buffer&, u64, u64, u64)>& fn, PartOutcome* outcome) const {
    if (outcome) *outcome = PartOutcome::Invalid;
    if (address > ImportedPageCoverage<>::Limit ||
        size == 0 || size > ImportedPageCoverage<>::Limit - address) return false;
    std::unique_lock lock{mutex, std::try_to_lock};
    if (!lock.owns_lock()) { if (outcome) *outcome = PartOutcome::Busy; return false; }
    if (outcome) *outcome = PartOutcome::MissingCoverage;
    const VAddr end = address + size;
    // Two passes: a missing suffix must not leave a partially recorded copy.
    for (VAddr cursor = address; cursor < end;) {
        const auto it = views.upper_bound(cursor);
        if (it == views.begin()) return false;
        const View& view = std::prev(it)->second;
        if (cursor < view.begin || cursor >= view.end || !view.wrapper) return false;
        if (cursor == address && end <= view.end) {
            if (outcome) *outcome = PartOutcome::Covered;
            if (!ready()) return false;
            fn(*view.wrapper, address - view.begin, 0, size);
            return true;
        }
        cursor = std::min(end, view.end);
    }
    if (outcome) *outcome = PartOutcome::Covered;
    if (!ready()) return false;
    for (VAddr cursor = address; cursor < end;) {
        const View& view = std::prev(views.upper_bound(cursor))->second;
        const u64 bytes = std::min(end, view.end) - cursor;
        fn(*view.wrapper, cursor - view.begin, cursor - address, bytes);
        cursor += bytes;
    }
    return true;
}

void DirectMemory::WarmImported(VAddr address, u64 size) {
    // Speculative preparation must not wait behind another worker or the recorder.
    // Bound both driver calls and time spent holding the shared view mutex. Large
    // Windows allocations stay on the normal path: warming a tiny texture must
    // not eagerly pin hundreds of MiB of unrelated backing.
    constexpr u64 MaxWarmViewBytes = 4ull << 20;
    constexpr u32 MaxWarmImports = 8;
    if (size == 0 || size > (64ull << 20) || address > ImportedPageCoverage<>::Limit ||
        size > ImportedPageCoverage<>::Limit - address) {
        return;
    }
    std::unique_lock lock{mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        return;
    }
#ifdef RYTY_GPU
    const u64 mapping_generation = Core::RytyMemory::MappingGeneration();
    if (failed_generation != mapping_generation) {
        failed.clear();
        failed_generation = mapping_generation;
    }
#endif
    const VAddr end = address + size;
    const auto started = std::chrono::steady_clock::now();
    u32 imported = 0;
    for (VAddr page = address & ~(PageSize - 1); page < end;) {
        const auto failure = failed.upper_bound(page);
        if (failure != failed.begin() && page < std::prev(failure)->second) {
            return;
        }
        auto it = views.upper_bound(page);
        if (it != views.begin()) {
            const auto& view = std::prev(it)->second;
            if (page >= view.begin && page < view.end) {
                page = view.end;
                continue;
            }
        }
        if (imported >= MaxWarmImports ||
            std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(1)) {
            return;
        }
        const VAddr base = GuestViewBase(page);
        const VAddr view_end = base ? GuestViewEnd(base, MaxWarmViewBytes) : 0;
        if (!base || view_end <= page || view_end - base > MaxWarmViewBytes) {
            return;
        }
        if (!ImportView(page, end)) {
            return;
        }
        ++imported;
    }
}

bool DirectMemory::EnsureImported(VAddr address, u64 size) {
    Native::SlowCallTrace<9> trace{Native::trace_guest_hash_calls, "ensure-imported",
        {"pending-lock", "completion-query", "destroy-wrapper", "free-memory", "unmap-alias",
         "recent-cache", "view-mutex", "mapping-generation", "lookup-import"}};
    trace.Next(0, address, size);
    // Imported-view destruction is queued at OnUnmap to the scheduler's
    // GPU-gated background runner. Never drain old imports on a draw's bind path.
    if (address > ImportedPageCoverage<>::Limit ||
        size > ImportedPageCoverage<>::Limit - address) return false;
    const VAddr end = address + size;
    trace.Next(5);
#ifndef NDEBUG
    static thread_local u64 coverage_hits{}, coverage_misses{};
#endif
    if (imported_coverage.Covers(address, size)) {
#ifndef NDEBUG
        if ((++coverage_hits & 65535) == 0) {
            LOG_INFO(Render_Vulkan,
                     "IMPORT-COVERAGE: {} completed-range hits without view lock, {} misses",
                     coverage_hits, coverage_misses);
        }
#endif
        return true;
    }
#ifndef NDEBUG
    ++coverage_misses;
#endif
    trace.Next(6);
    std::scoped_lock lk{mutex};
#ifdef RYTY_GPU
    // OnUnmap is reported before the host remap. A lookup during that window can
    // fail, so discard negative results after the completed mapping is reported too.
    trace.Next(7);
    const u64 mapping_generation = Core::RytyMemory::MappingGeneration();
    if (failed_generation != mapping_generation) {
        failed.clear();
        failed_generation = mapping_generation;
    }
#endif
    trace.Next(8);
    for (VAddr page = address & ~(PageSize - 1); page < end;) {
        // The bitmap skips already imported prefixes even when a later view in
        // the request is missing. Retry each missing view through the old path.
        if (const auto missing = imported_coverage.FirstMissing(page, end - page)) {
            page = *missing;
            if (page >= end) break;
        }
        // A failed view is retried by many draws (and again by the gather fallback).
        // Its bounds are already known: avoid querying Windows for its base each time.
        const auto failure = failed.upper_bound(page);
        if (failure != failed.begin() && page < std::prev(failure)->second) {
            return false;
        }
        auto it = views.upper_bound(page);
        if (it != views.begin()) {
            const auto& view = std::prev(it)->second;
            if (page >= view.begin && page < view.end) {
                page = view.end;
                continue;
            }
        }
        if (!ImportView(page, end)) {
            return false;
        }
    }
    return true;
}

void DirectMemory::OnUnmap(VAddr address, u64 size) {
    std::unique_lock lk{mutex};
    imported_view_epoch.fetch_add(1, std::memory_order_acq_rel);
    {
    auto invalidate_coverage = imported_coverage.Invalidate();
    const VAddr end = address + size;
    if (TrackParallelDirect()) direct_coherence.Remove(this,address,end);
    const u64 tick = scheduler.CurrentTick();
    OnUnmapImportedViews(views, address, end, held_native_releases, hold_native_releases,
        [&](View& view) {
        imported_coverage.Clear(view.begin, view.end);
        UnmapPages(view.begin, view.end);
        imported_bytes -= view.end - view.begin;
        // DIAG-132: views dropped while GPU work may still read them.
        static std::atomic<u32> dropped{};
        if (const u32 n = ++dropped; n <= 40 || n % 100 == 0) {
            LOG_WARNING(Render_Vulkan,
                        "DIAG-132: view {:#x}+{:#x} dropped (unmap {:#x}+{:#x}, tick {}, GPU at "
                        "{}, {} dropped)",
                        view.begin, view.end - view.begin, address, size, tick,
                        scheduler.CurrentTick(), n);
        }
        }, [&](auto& held) { RetireLeasesAfterSplice(held, scheduler.CurrentTick()); });
    // Even a partial unmap can replace part of a failed view with importable memory.
    std::erase_if(failed, [&](const auto& view) {
        return view.first < end && (view.second > address || view.first >= address);
    });
    }
    // Invalidate the GPU page table and coverage before publishing releases.
    // The runner waits for its captured CurrentTick before invoking callbacks;
    // a later tick is conservative if the recorder advanced during this unmap.
    // On queue failure the owners remain in held_native_releases until a later
    // successful retirement or GPU-idle teardown.
}

bool DirectMemory::ViewsOver(VAddr address, u64 size) {
    std::scoped_lock lk{mutex};
    const VAddr end = address + std::max<u64>(size, 1);
    std::erase_if(failed, [&](const auto& view) { return view.first < end && view.second > address; });
    auto it = views.upper_bound(address);
    if (it != views.begin() && std::prev(it)->second.end > address) {
        return true;
    }
    return it != views.end() && it->first < end;
}

// FIX-205: views dropped at an unmap are leased (ImportedViewLease) and, in native mode, held in
// held_native_releases until the native list that may use them is spliced
// (HoldNativeReleases / ReleaseHeldAfterSplice). The static pair below is kept for callers of the
// earlier interface; the leases already give them the same guarantee.
void DirectMemory::HoldReleasesForSplice() {}

void DirectMemory::ReleaseHeldAfter(u64) {}

DirectMemory::Stats DirectMemory::GetStats() const {
    std::scoped_lock lk{mutex};
    return {views.size(), imported_bytes.load(), failed_count.load()};
}

void DirectMemory::Release(View& view) {
    view.wrapper.reset();
    view.lease.reset();
}

} // namespace VideoCore
