#define NOMINMAX
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <mutex>
#include <thread>
#include <vector>
#include "video_core/renderer_vulkan/immutable_upload_memo.h"

void Require(bool ok,const char* why){if(!ok){std::printf("FAIL %s\n",why);std::exit(1);}}
using Memo=Vulkan::ImmutableUploadMemo<unsigned,6>;
static Memo memo;
unsigned sequence=0,hits=0;
std::vector<std::vector<unsigned char>> snapshots(1);
auto hash=[](const void* data,size_t size){
 auto* p=static_cast<const unsigned char*>(data);uint64_t h=1469598103934665603ull;
 for(size_t i=0;i<size;++i){h^=p[i];h*=1099511628211ull;}return h;
};
template<class Hash> unsigned Upload(unsigned stage,std::span<const unsigned char> data,Hash hasher){
 auto ticket=memo.Find(stage,data,hasher);
 if(ticket.binding){++hits;Require(snapshots[*ticket.binding]==std::vector<unsigned char>(data.begin(),data.end()),"cached binding matches complete immutable bytes");return *ticket.binding;}
 unsigned id=++sequence;snapshots.emplace_back(data.begin(),data.end());memo.Store(ticket,stage,data,id);return id;
}
int main(){
 std::array<unsigned char,16> a{},b{};b[3]=7;
 auto first=Upload(0,a,hash);Upload(0,b,hash);
 Require(Upload(0,a,hash)==first,"A/B/A reuses A across intervening draw");
 Require(Upload(1,a,hash)!=first,"stages remain isolated");
 auto collision=[](const void*,size_t){return uint64_t{0};};
 memo.Clear();
 for(unsigned i=0;i<10000;++i){std::array<unsigned char,64> data{};data[0]=i%31;data[63]=(i/7)%19;Upload(i%6,data,collision);}
 memo.Clear();auto old=Upload(2,a,hash);memo.Clear();Require(Upload(2,a,hash)!=old,"list retirement cannot reuse old upload binding");
 // Exact equality on page-end inputs, including the vectorized comparison path.
 auto* guarded=static_cast<unsigned char*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));Require(guarded!=nullptr,"guard allocation");
 DWORD previous;Require(VirtualProtect(guarded+4096,4096,PAGE_NOACCESS,&previous),"guard page");
 for(size_t size=1;size<=4096;++size){auto* data=guarded+4096-size;for(size_t i=0;i<size;++i)data[i]=static_cast<unsigned char>(i*11+size);auto one=Upload(3,{data,size},hash);Require(Upload(3,{data,size},hash)==one,"guarded warm hit");data[size-1]^=1;Require(Upload(3,{data,size},hash)!=one,"same pointer mutated bytes cannot hit");}
 VirtualFree(guarded,0,MEM_RELEASE);
 std::vector<unsigned char> large(65537,0x77);auto oversize=memo.Find(0,large,hash);Require(!oversize.binding,"oversize bypass");memo.Store(oversize,0,large,1);Require(!memo.Find(0,large,hash).binding,"oversize not retained");
 for(unsigned i=0;i<512;++i){std::vector<unsigned char> bytes(60000+i%5537,static_cast<unsigned char>(i));Upload(i%6,bytes,hash);Require(memo.RetainedBytes()<=128ull*65536,"retained CPU references stay within 8 MiB");}
 // Repeated material blocks: previous-only reuse is zero; this cache reuses the working set.
 memo.Clear();unsigned before=hits;
 for(unsigned i=0;i<1000;++i){std::array<unsigned char,128> bytes{};bytes[0]=i%8;Upload(0,bytes,hash);}
 Require(hits-before==992,"eight-material cycle reuses 992 of 1000 uploads");
 // Snapshot memo has a separate domain, eight entries and a 1 MiB per-entry cap.
 Vulkan::ImmutableUploadMemo<unsigned,1,8,1024*1024> snapshot_memo;
 std::vector<unsigned char> guest(70001,0x35), captured=guest;
 auto ticket=snapshot_memo.Find(0,captured,collision);
 guest[0]^=1; // Guest changes after the read: upload and memo still use the captured bytes.
 auto uploaded=captured;
 snapshot_memo.Store(ticket,0,captured,17);
 Require(snapshot_memo.Find(0,uploaded,collision).binding==17,"captured bytes reused after source mutation");
 Require(!snapshot_memo.Find(0,guest,collision).binding,"one byte mutation and forced hash collision miss");
 uploaded.back()^=1;
 Require(!snapshot_memo.Find(0,uploaded,collision).binding,"large-buffer last byte mutation misses");
 snapshot_memo.Clear();
 Require(!snapshot_memo.Find(0,captured,collision).binding,"snapshot binding expires on list clear");
 std::mutex stream_mutex;
 std::thread recorder([&]{std::scoped_lock lock{stream_mutex};auto item=snapshot_memo.Find(0,captured,collision);snapshot_memo.Store(item,0,captured,18);});
 recorder.join();
 std::thread retire([&]{std::scoped_lock lock{stream_mutex};snapshot_memo.Clear();});
 retire.join();
 std::thread resume([&]{std::scoped_lock lock{stream_mutex};Require(!snapshot_memo.Find(0,captured,collision).binding,"another thread retiring stream invalidates recorder binding");});
 resume.join();
 for(unsigned i=0;i<16;++i){
  std::vector<unsigned char> bytes(1024*1024,static_cast<unsigned char>(i));
  auto item=snapshot_memo.Find(0,bytes,collision);snapshot_memo.Store(item,0,bytes,i);
  Require(snapshot_memo.RetainedBytes()<=8ull*1024*1024,"snapshot memo remains within 8 MiB");
 }
 std::vector<unsigned char> too_large(1024*1024+1,0x44);
 auto rejected=snapshot_memo.Find(0,too_large,collision);snapshot_memo.Store(rejected,0,too_large,1);
 Require(!snapshot_memo.Find(0,too_large,collision).binding,"over 1 MiB bypass");
 std::printf("PASS: 10000 forced-collision cases; 4096 guarded lengths and mutation cases; stage/list isolation; 8 MiB bound; material cycle 992/1000 reused\n");
 std::printf("PASS: snapshot >64 KiB, collision, mutation, cross-thread clear and 8 MiB cap\n");
}
