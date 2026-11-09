"""Compile the actual direct-import measurement helper with a deterministic trace stub."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
src = (root / 'src/video_core/buffer_cache/direct_memory.cpp').read_text(encoding='utf-8')
start = src.index('struct ImportMeasure;')
end = src.index('// Every view insertion', start)
prefix = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
using u64 = std::uint64_t;
static bool open=true, locked=false; static u64 clock_ticks=10, emissions=0;
namespace ImageLife {
bool Enabled(const char*) { return open; }
u64 Timestamp() { return open ? ++clock_ticks : 0; }
u64 SlowThresholdTicks() { return 1; }
struct Scope {
    Scope(const char*,u64=0,u64=0,u64=0,u64=0,u64=0) { assert(!locked); ++emissions; }
    bool Active() const { return true; } u64 Id() const { return 5; }
    void Finish(u64=0,u64=0) {}
};
}
'''
suffix = r'''
struct Lock { Lock(){locked=true;} ~Lock(){locked=false;} };
void attempt(bool imported) {
    ImportMeasure measure{2,1,2,3}; measure.Waiting(); Lock lock; measure.Acquired();
    ImportHoldEnd hold{measure};
    if(imported) { ImportViewMeasure view; view.bytes=42; view.success=true;
        ImportPhase phase{&ImportMeasure::allocate_ticks}; }
}
int main() {
    attempt(true); assert(emissions==4 && !locked && current_import_measure==nullptr);
    attempt(false); assert(emissions==8 && !locked && current_import_measure==nullptr);
    open=false; attempt(true); assert(emissions==8 && current_import_measure==nullptr);
    open=true;
    { ImportMeasure outer{1,0,0,0}; assert(current_import_measure==&outer);
      { ImportMeasure inner{2,0,0,0}; assert(current_import_measure==&inner); }
      assert(current_import_measure==&outer); }
    assert(current_import_measure==nullptr);
}
'''
with tempfile.TemporaryDirectory() as directory:
    test = pathlib.Path(directory) / 'test.cpp'
    exe = pathlib.Path(directory) / 'test.exe'
    test.write_text(prefix + src[start:end] + suffix, encoding='utf-8')
    subprocess.run([sys.argv[1], '-std=c++20', '-O2', '-static', str(test), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('direct import measurement test passed')
