from pathlib import Path
import sys
src=Path(sys.argv[1]).read_text()
a=src.index('void NativeRenderer::OnCpuStack(');b=src.index('\nvoid NativeRenderer::OnCpuWrite(',a)
Path(sys.argv[2]).write_text(src[a:b])
a=src.index('    if (!has_pending_region_writes.exchange',src.index('void NativeRenderer::SortRegionWrites()'));b=src.index('    for (const auto& [begin, end] : writes)',a)
Path(sys.argv[3]).write_text(src[a:b])
a=src.index('        // Copies are already recorded;');b=src.index('        if(observation==MirrorWorker::Observation::Reused)',a)
Path(sys.argv[4]).write_text(src[a:b])

a=src.index('void NativeRenderer::DropMirrors(');b=src.index('\nvoid NativeRenderer::RefreshRegion(',a)
Path(sys.argv[5]).write_text(src[a:b])
a=src.index('void NativeRenderer::UnwatchPages(');b=src.index('\nvoid NativeRenderer::ReleaseUnusedWatches(',a)
Path(sys.argv[6]).write_text(src[a:b])
a=b+1;b=src.index('\nvoid NativeRenderer::OnCpuStack(',a)
Path(sys.argv[7]).write_text(src[a:b])
