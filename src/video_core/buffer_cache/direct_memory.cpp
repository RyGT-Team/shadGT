// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include "ryty_gpu/native/memory_range_runs.h"
#include "ryty_gpu/native/epoch_lookup_memo.h"
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/direct_memory.h"
#include "video_core/buffer_cache/defer_import_releases.h"
#include "video_core/buffer_cache/import_cleanup_worker.h"
#include "ryty_gpu/native/slow_call_trace.h"
#include "ryty_gpu/native/direct_coherence_registry.h"
#include "ryty_gpu/native/parallel_direct_capability.h"
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

struct PendingRelease {
    void* alias;
    VkDeviceMemory memory;
    std::shared_ptr<Buffer> wrapper; // owns the VkBuffer (D6)
};

} // namespace

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
    std::scoped_lock lk{mutex};
    static thread_local Native::EpochRangeMemo<const Buffer*> memo;
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

bool DirectMemory::ImportView(VAddr address) {
    Native::SlowCallTrace<8> trace{Native::trace_guest_hash_calls, "import-guest-view",
        {"guest-view", "map-alias", "host-properties", "allocate-memory", "create-bind-buffer",
         "device-address", "map-pages", "store-view"}};
    trace.Next(0, address, 0);
#ifdef _WIN32
    const VAddr base = GuestViewBase(address);
    if (base == 0) {
        return false;
    }
    if (failed.contains(base)) {
        return false;
    }
    const VAddr end = GuestViewEnd(base, 1ULL << 32);
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
    MapPages(base, end, device_address);
    trace.Next(7);
    auto wrapper = std::make_shared<Buffer>(instance, base, end - base, vk::Buffer{buffer});
    views.emplace(base, View{base, end, alias, memory, buffer, device_address, std::move(wrapper)});
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

bool DirectMemory::TryForEachPart(
    VAddr address, u64 size, const std::function<bool()>& ready,
    const std::function<void(const Buffer&, u64, u64, u64)>& fn) const {
    if (address > ImportedPageCoverage<>::Limit ||
        size == 0 || size > ImportedPageCoverage<>::Limit - address) return false;
    std::unique_lock lock{mutex, std::try_to_lock};
    if (!lock.owns_lock()) return false;
    const VAddr end = address + size;
    // Two passes: a missing suffix must not leave a partially recorded copy.
    for (VAddr cursor = address; cursor < end;) {
        const auto it = views.upper_bound(cursor);
        if (it == views.begin()) return false;
        const View& view = std::prev(it)->second;
        if (cursor < view.begin || cursor >= view.end || !view.wrapper) return false;
        cursor = std::min(end, view.end);
    }
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
        if (!ImportView(page)) {
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
    static thread_local u64 coverage_hits{}, coverage_misses{};
    if (imported_coverage.Covers(address, size)) {
        if ((++coverage_hits & 65535) == 0) {
            LOG_INFO(Render_Vulkan,
                     "IMPORT-COVERAGE: {} completed-range hits without view lock, {} misses",
                     coverage_hits, coverage_misses);
        }
        return true;
    }
    ++coverage_misses;
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
        if (!ImportView(page)) {
            return false;
        }
    }
    return true;
}

void DirectMemory::OnUnmap(VAddr address, u64 size) {
    std::vector<PendingRelease> releases;
    std::unique_lock lk{mutex};
    imported_view_epoch.fetch_add(1, std::memory_order_acq_rel);
    {
    auto invalidate_coverage = imported_coverage.Invalidate();
    const VAddr end = address + size;
    if (TrackParallelDirect()) direct_coherence.Remove(this,address,end);
    auto it = views.upper_bound(address);
    if (it != views.begin()) {
        --it;
    }
    const u64 tick = scheduler.CurrentTick();
    while (it != views.end() && it->first < end) {
        View& view = it->second;
        if (view.end <= address) {
            ++it;
            continue;
        }
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
        releases.push_back({view.alias, view.memory, std::move(view.wrapper)});
        it = views.erase(it);
    }
    // Even a partial unmap can replace part of a failed view with importable memory.
    std::erase_if(failed, [&](const auto& view) {
        return view.first < end && (view.second > address || view.first >= address);
    });
    }
    // Invalidate the GPU page table and coverage before publishing releases.
    // The runner waits for its captured CurrentTick before invoking callbacks;
    // a later tick is conservative if the recorder advanced during this unmap.
    lk.unlock();
    const auto device = instance.GetDevice();
    const auto unmap_alias = UnmapAlias();
    DeferImportReleases(std::move(releases),
        [this, cleanup = import_cleanup](auto callback) {
            // The scheduler gate establishes GPU completion. Keep its fence
            // runner free: driver destruction belongs to a separate worker.
            scheduler.DeferPriorityOperation(
                [cleanup, callback = std::move(callback)]() mutable {
                    cleanup->Enqueue(std::move(callback));
                });
        },
        [device, unmap_alias](PendingRelease& release) {
            release.wrapper.reset();
            VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeMemory(device, release.memory, nullptr);
            unmap_alias(release.alias);
        });
}

DirectMemory::Stats DirectMemory::GetStats() const {
    std::scoped_lock lk{mutex};
    return {views.size(), imported_bytes.load(), failed_count.load()};
}

void DirectMemory::Release(View& view) {
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    view.wrapper.reset();
    d.vkFreeMemory(instance.GetDevice(), view.memory, nullptr);
    UnmapAlias()(view.alias);
}

} // namespace VideoCore
