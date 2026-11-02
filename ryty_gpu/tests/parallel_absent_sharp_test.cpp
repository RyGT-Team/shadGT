#include <array>
#include <cassert>
#include <cstdint>
#include <random>
#include <span>
#include <vector>
#include "shader_recompiler/resource.h"
#include "ryty_gpu/native/prepared_direct_absent.h"

struct Action { std::size_t stage_index{},stage_local{}; u32 absent_sharp_offset{}; bool absent_sharp{}; };
struct Copy { bool present{true}; std::vector<u32> flattened; u64 pgm_base{}; };
struct Prepared { std::array<Copy,1> stage_copies; };
struct Stage { std::vector<Shader::BufferResource> buffers; };
struct Rasterizer { u64 draw_stage_install_serial{}; void Install() {
    auto& draw_stage_install_serial=this->draw_stage_install_serial;
#include "absent-install.inc"
} };

void Produce(Action& action, std::array<Stage*,1>& stages, Prepared* p) {
#include "absent-producer.inc"
}
bool Consume(Action* action, const Stage& stage, Rasterizer& r, u64 token,
             bool trusted=true, bool from_prepared=true, bool worker_identity=true,
             bool known_stage_schema=true, bool planned=true) {
    const auto& desc=stage.buffers[0]; const std::size_t local=0;
    const u64 trusted_install_serial=token;
    const auto planned_stage=[&]() -> const Stage* {return planned ? &stage : nullptr;};
#include "absent-recorder.inc"
    return certified_absent;
}
struct Snapshot { std::span<const u32> flattened_ud_buf; u64 pgm_base{}; };
int main() {
    Stage stage; stage.buffers.resize(1); auto& desc=stage.buffers[0];
    desc.sharp_fetch.summary=decltype(desc.sharp_fetch)::Summary::SingleLoad;
    desc.sharp_fetch.offsets[0]=1;
    Prepared prepared; auto& copy=prepared.stage_copies[0];copy.flattened.resize(6);
    std::array<Stage*,1> stages{&stage}; Rasterizer r; r.Install(); const u64 token=r.draw_stage_install_serial;
    std::mt19937 rng{123456};
    for(unsigned i=0;i<20000;++i) {
        for(auto& word:copy.flattened) word=rng();
        if(i%3==0) for(unsigned j=1;j<5;++j)copy.flattened[j]=0;
        Action action{};Produce(action,stages,&prepared);
        auto sharp=desc.GetSharp(Snapshot{copy.flattened,copy.pgm_base});
        const bool expected=!sharp.base_address || !sharp.GetSize();
        assert(action.absent_sharp==expected);
        assert(Consume(&action,stage,r,token)==expected);
        const auto selected=Consume(&action,stage,r,token) ? AmdGpu::Buffer::Null() : sharp;
        std::array<std::byte,32> table{};
        for(unsigned variant=0;variant<12;++variant) {
            table.fill(std::byte{});
            if(variant<4) table[16+variant*4]=std::byte{1};
            const bool eligible=variant!=4, device=variant!=5, null_buffer=variant!=6;
            const u64 offset=variant==7 ? 1 : 0, range=variant==8 ? 1 : UINT64_MAX;
            const size_t count=variant==9 ? 2 : 1, index=variant==10 ? 1 : 0;
            const auto bytes=std::span<const std::byte>{table}.first(variant==11 ? 31 : 32);
            const auto original=Native::Parallel::IsPreparedAbsentDirectSlot(bytes,count,index,eligible,device,null_buffer,offset,range,sharp.base_address,sharp.GetSize());
            const auto candidate=Native::Parallel::IsPreparedAbsentDirectSlot(bytes,count,index,eligible,device,null_buffer,offset,range,selected.base_address,selected.GetSize());
            assert(original==candidate);
        }
        if(expected) {
            assert(!Consume(&action,stage,r,token,false));
            assert(!Consume(&action,stage,r,token,true,false));
            assert(!Consume(&action,stage,r,token,true,true,false));
            assert(!Consume(&action,stage,r,token,true,true,true,false));
            assert(!Consume(&action,stage,r,token,true,true,true,true,false));
            ++desc.sharp_fetch.offsets[0];assert(!Consume(&action,stage,r,token));--desc.sharp_fetch.offsets[0];
            desc.post_op=Shader::SharpFetchPostOp::OffsetByProgramBase;assert(!Consume(&action,stage,r,token));desc.post_op=Shader::SharpFetchPostOp::None;
            desc.sharp_fetch.summary=decltype(desc.sharp_fetch)::Summary::MultiLoad;assert(!Consume(&action,stage,r,token));desc.sharp_fetch.summary=decltype(desc.sharp_fetch)::Summary::SingleLoad;
            desc.is_written=true;assert(!Consume(&action,stage,r,token));desc.is_written=false;
            desc.is_formatted=true;assert(!Consume(&action,stage,r,token));desc.is_formatted=false;
            desc.buffer_type=Shader::BufferType::Flatbuf;assert(!Consume(&action,stage,r,token));desc.buffer_type=Shader::BufferType::Guest;
            action.stage_local=1;assert(!Consume(&action,stage,r,token));
        }
    }
    for(auto& word:copy.flattened)word=0;
    Action action{};Produce(action,stages,&prepared);assert(action.absent_sharp && Consume(&action,stage,r,token));
    AmdGpu::Buffer nonnull{};nonnull.base_address=32;nonnull.num_records=64;
    std::memcpy(copy.flattened.data()+1,&nonnull,sizeof(nonnull));
    r.Install();assert(!Consume(&action,stage,r,token)); // models a later, failed or reentrant entry
    auto changed=desc.GetSharp(Snapshot{copy.flattened,copy.pgm_base});
    std::array<std::byte,32> zeros{};
    assert(!Native::Parallel::IsPreparedAbsentDirectSlot(zeros,1,0,true,true,true,0,UINT64_MAX,changed.base_address,changed.GetSize()));
    r.draw_stage_install_serial=UINT64_MAX-1;r.Install();assert(r.draw_stage_install_serial==UINT64_MAX);
    r.Install();assert(r.draw_stage_install_serial==UINT64_MAX);assert(!Consume(&action,stage,r,UINT64_MAX));
    desc.sharp_fetch.offsets[0]=3;action={};Produce(action,stages,&prepared);assert(!action.absent_sharp); // out of bounds
    desc.sharp_fetch.offsets[0]=2;action={};Produce(action,stages,&prepared); // exact end is valid
    copy.present=false;action={};Produce(action,stages,&prepared);assert(!action.absent_sharp);copy.present=true;
    action={};action.stage_index=1;Produce(action,stages,&prepared);assert(!action.absent_sharp);
    desc.sharp_fetch.offsets[0]=1;
    for(const auto base:{u64{0},u64{1}})for(const auto size:{u32{0},u32{1}}) {
        AmdGpu::Buffer value{};value.base_address=base;value.num_records=size;
        std::memcpy(copy.flattened.data()+1,&value,sizeof(value));
        action={};Produce(action,stages,&prepared);assert(action.absent_sharp==(!base || !size));
    }
}
