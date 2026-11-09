// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <tuple>
#include <vector>
using u8=std::uint8_t;using u32=std::uint32_t;using u64=std::uint64_t;
namespace vk {struct DescriptorBufferInfo {unsigned buffer{};u64 offset{},range{};};}
struct Buffer {unsigned handle{7};bool mapped{true};u64 capacity{64};bool Mapped()const{return mapped;}u64 Size()const{return capacity;}};
static bool AllocationProof(const Buffer& buffer,unsigned handle,u64 offset,u64 size) {
    bool value=false;bool* backing_proven=&value;
#include "upload-backing-proof.inc"
    return value;
}
struct Context {bool active{true};bool Valid(u64 state)const{return active && state==1;}};
struct Upload {u32 info{},offset{},size{};int flat_stage{-1};};
struct Prepared {u64 upload_receipt_incarnation{},proof_mode_state{1};std::shared_ptr<Context> proof_context=std::make_shared<Context>();std::vector<Upload> uploads;std::vector<u8> upload_bytes;struct {std::vector<vk::DescriptorBufferInfo> buffers;} bindings;};
struct Stage {std::vector<u32> flattened_ud_buf;};
struct Owner {
    struct Stream {u64 upload_incarnation{1};} stream;std::array<u8,64> data{};bool valid{true},invalidate{};
    std::tuple<unsigned,u64,u8*> Allocate(u64,u64,bool* proof) {if(proof)*proof=valid;if(invalidate)Bump();return {7,0,data.data()};}
    vk::DescriptorBufferInfo UploadFlatData(u32,const u8*,u32 size,bool* proof) {if(proof)*proof=valid;if(invalidate)Bump();return {7,0,size};}
    void Bump(){auto& s=stream;
#include "upload-incarnation-bump.inc"
    }
    void Commit(Prepared& p,bool trusted) {u64 alignment=16;Stage stage{{0,0,0,0}};std::array<Stage*,1> stages{&stage};
#include "upload-commit-proof.inc"
    }
};
int main() {
    Buffer b;assert(AllocationProof(b,7,0,64));assert(!AllocationProof(b,0,0,1));assert(!AllocationProof(b,7,0,0));
    assert(!AllocationProof(b,7,63,2));assert(!AllocationProof(b,7,UINT64_MAX,1));b.mapped=false;assert(!AllocationProof(b,7,0,1));
    Owner o;Prepared p;p.upload_bytes.resize(16);p.bindings.buffers.resize(2);p.uploads={{0,0,8,-1},{1,0,8,0}};
    o.Commit(p,true);assert(p.upload_receipt_incarnation==1);
    o.valid=false;o.Commit(p,true);assert(!p.upload_receipt_incarnation);o.valid=true;
    o.Commit(p,false);assert(!p.upload_receipt_incarnation);o.invalidate=true;o.Commit(p,true);assert(!p.upload_receipt_incarnation);o.invalidate=false;
    o.Commit(p,true);auto old=p.upload_receipt_incarnation;assert(old==o.stream.upload_incarnation);o.Bump();assert(old!=o.stream.upload_incarnation);
    o.stream.upload_incarnation=UINT64_MAX;o.Bump();assert(o.stream.upload_incarnation==0);o.Bump();assert(o.stream.upload_incarnation==0);
    o.Commit(p,true);assert(!p.upload_receipt_incarnation);
}
