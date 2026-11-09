#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_command_stream.h"

namespace Vulkan::SceneTrace {
#ifndef NDEBUG
constexpr size_t Limit = 8192;
constexpr u32 NoQuery = ~u32{};
struct Row {
    u64 frame{}, program{}, companion{}, cpu_ns{};
    const char* kind{};
    u32 first_query = NoQuery;
    std::array<u64, 8> phases{}, record{};
    bool phases_measured{}, phases_valid{};
};
struct State {
    std::vector<Row> rows;
    vk::QueryPool pool{};
    u32 queries{}, bits{}, dropped{};
    double period{};
    bool attempted{};
};
inline State state;
inline thread_local size_t current = Limit;
inline u64 Id() { return current < state.rows.size() ? current + 1 : 0; }
class CpuScope {
public:
    CpuScope(bool enabled, u64 frame, const char* kind,
             std::span<const u64> phases = {}, std::span<const u64> record = {},
             const u64* reset_generation = nullptr)
        : old(current), phases(phases), record(record), reset_generation(reset_generation) {
        current = Limit;
        if (!enabled) return;
        if (state.rows.size() == Limit) { ++state.dropped; return; }
        if (state.rows.empty()) state.rows.reserve(Limit);
        current = index = state.rows.size();
        state.rows.push_back(Row{.frame=frame, .kind=kind});
        state.rows[index].phases_measured = reset_generation && !phases.empty() && !record.empty();
        if (reset_generation) generation_before = *reset_generation;
        for (size_t i=0; i<std::min(phases.size(),before.size()); ++i) before[i]=phases[i];
        for (size_t i=0; i<std::min(record.size(),rec_before.size()); ++i) rec_before[i]=record[i];
        start=std::chrono::steady_clock::now();
    }
    ~CpuScope() {
        if (index < state.rows.size()) {
            auto& row=state.rows[index];
            row.cpu_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now()-start).count();
            row.phases_valid = row.phases_measured && *reset_generation == generation_before;
            for (size_t i = 0; row.phases_valid && i < std::min(phases.size(), row.phases.size()); ++i)
                if (phases[i] < before[i]) row.phases_valid = false;
            for (size_t i = 0; row.phases_valid && i < std::min(record.size(), row.record.size()); ++i)
                if (record[i] < rec_before[i]) row.phases_valid = false;
            if (row.phases_valid) {
                for(size_t i=0;i<std::min(phases.size(),row.phases.size());++i) row.phases[i]=phases[i]-before[i];
                for(size_t i=0;i<std::min(record.size(),row.record.size());++i) row.record[i]=record[i]-rec_before[i];
            }
        }
        current=old;
    }
    void Programs(u64 program,u64 companion=0) {
        if(index<state.rows.size()){state.rows[index].program=program;state.rows[index].companion=companion;}
    }
private:
    size_t index=Limit,old;
    std::span<const u64> phases,record;
    const u64* reset_generation{};
    u64 generation_before{};
    std::array<u64,8> before{},rec_before{};
    std::chrono::steady_clock::time_point start;
};
// Caller must be outside rendering. Reset once, before any captured timestamp.
inline void Prepare(vk::Device device,CmdBuf cmd,u32 bits,double period) {
    if (!Id() || state.attempted) return;
    state.attempted=true;state.bits=bits;state.period=period;
    if(!bits || period<=0) return;
    auto [result,pool]=device.createQueryPool(vk::QueryPoolCreateInfo{
        .queryType=vk::QueryType::eTimestamp,.queryCount=u32(Limit*2)});
    if(result!=vk::Result::eSuccess)return;
    state.pool=pool;cmd.resetQueryPool(pool,0,u32(Limit*2));
}
inline void GpuBegin(CmdBuf cmd) {
    if(!Id() || !state.pool)return;
    auto& row=state.rows[current];
    if(state.queries+2>Limit*2)return;
    row.first_query=state.queries;state.queries+=2;
    cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,state.pool,row.first_query);
}
inline void GpuEnd(CmdBuf cmd) {
    if(Id() && state.pool && state.rows[current].first_query!=NoQuery)
        cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,state.pool,state.rows[current].first_query+1);
}
// Caller has submitted and finished the captured GPU work. Never wait for unused queries.
inline void Save(const std::filesystem::path& dir,vk::Device device) {
    std::vector<std::array<u64,2>> results(state.queries);
    vk::Result result=vk::Result::eNotReady;
    if(state.pool && state.queries) result=device.getQueryPoolResults(state.pool,0,state.queries,
        results.size()*sizeof(results[0]),results.data(),sizeof(results[0]),
        vk::QueryResultFlagBits::e64|vk::QueryResultFlagBits::eWithAvailability);
    if(auto* f=std::fopen((dir/"pass-profile.csv").string().c_str(),"w")) {
        std::fprintf(f,"id,frame,kind,program,companion,cpu_ns,gpu_span_ns,gpu_available,phase_pipeline_ns,phase_targets_ns,phase_buffers_ns,phase_textures_ns,phase_sources_ns,phase_vertices_ns,phase_prepare_ns,phase_record_ns,record_barrier_ns,record_begin_ns,record_dynamic_ns,record_vertex_ns,record_pipeline_ns,record_bindings_ns,record_draw_ns,record_notes_ns,phases_valid\n");
        for(size_t i=0;i<state.rows.size();++i){const auto& row=state.rows[i];double ns=0;bool available=false;
            if(row.first_query!=NoQuery && (result==vk::Result::eSuccess || result==vk::Result::eNotReady)){
                auto a=results[row.first_query],b=results[row.first_query+1];available=a[1]&&b[1];
                const u64 mask=state.bits>=64?std::numeric_limits<u64>::max():((u64{1}<<state.bits)-1);
                if(available) ns=double((b[0]-a[0])&mask)*state.period;
            }
            std::fprintf(f,"%zu,%llu,%s,0x%llx,0x%llx,%llu,%.3f,%u",i+1,row.frame,row.kind,row.program,row.companion,row.cpu_ns,ns,unsigned(available));
            for(auto x:row.phases) { if(row.phases_valid) std::fprintf(f,",%llu",x); else std::fputc(',',f); }
            for(auto x:row.record) { if(row.phases_valid) std::fprintf(f,",%llu",x); else std::fputc(',',f); }
            std::fprintf(f,",%u",unsigned(row.phases_valid));
            std::fputc('\n',f);
        }std::fclose(f);
    }
    if(auto* f=std::fopen((dir/"trace-status.txt").string().c_str(),"w")){
        const auto invalid = std::ranges::count_if(state.rows, [](const Row& row) { return row.phases_measured && !row.phases_valid; });
        const auto unavailable = std::ranges::count_if(state.rows, [](const Row& row) { return !row.phases_measured; });
        std::fprintf(f,"rows=%zu dropped=%u queries=%u timestamp_bits=%u timestamp_period_ns=%.9g query_result=%d invalid_phase_rows=%zu unavailable_phase_rows=%zu\nGPU spans may overlap and include dependencies; CPU times include diagnostic overhead. F8 bypasses worker-prepared bindings. These are not normal-play frame timings.\n",state.rows.size(),state.dropped,state.queries,state.bits,state.period,int(result),size_t(invalid),size_t(unavailable));std::fclose(f);
    }
    if(state.pool)device.destroyQueryPool(state.pool);
    state={};current=Limit;
}
#else
struct State { vk::QueryPool pool{}; bool attempted{}; };
inline State state;
inline constexpr u64 Id() noexcept { return 0; }
class CpuScope {
public:
    CpuScope(bool, u64, const char*, std::span<const u64> = {}, std::span<const u64> = {}, const u64* = nullptr) noexcept {}
    void Programs(u64, u64 = 0) noexcept {}
};
inline void Prepare(vk::Device, CmdBuf, u32, double) noexcept {}
inline void GpuBegin(CmdBuf) noexcept {}
inline void GpuEnd(CmdBuf) noexcept {}
inline void Save(const std::filesystem::path&, vk::Device) noexcept {}
#endif

}
