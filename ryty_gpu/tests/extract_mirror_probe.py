from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text(encoding='utf-8')
merge_start=source.index('    std::ranges::sort(pending);',source.index('void NativeRenderer::MakePendingMirrors()'))
a=source.index('    // Resume at the saved address, including a range grown over that address.')
b=source.index('    static const u64 budget = [] {',a)
c=source.index('        using PartOutcome = VideoCore::DirectMemory::PartOutcome;',b)
d=source.index('        const auto promotion_start =',c)
# Deterministic monotonic clock controls the actual extracted production branch.
probe=source[c:d].replace('std::chrono::steady_clock::now()', 'FakeNow()')
Path(sys.argv[2]).write_text(source[a:b],encoding='utf-8')
Path(sys.argv[3]).write_text(probe,encoding='utf-8')

Path(sys.argv[4]).write_text(source[merge_start:a],encoding='utf-8')

start=source.index('            if (request.generation != unmap_generation.load()) return {};',source.index('mirror_preparation = std::make_unique'))
end=source.index('            return device->CreateBuffer(',start)
Path(sys.argv[5]).write_text(source[start:end],encoding='utf-8')
