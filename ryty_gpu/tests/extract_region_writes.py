from pathlib import Path
import hashlib, sys
source = Path(sys.argv[1]).read_text(encoding='utf-8')
start = source.index('void NativeRenderer::SortRegionWrites()')
loop = source.index('        auto region_it = regions.lower_bound(begin);', start)
tail = source.index('        // FIX-173: copies of pages no region holds are not needed.', loop)
end = source.index('    if (mirror_writes.size()', tail)
assert hashlib.sha256(source[tail:end].encode()).hexdigest() == '244793b9b92222553e3bcc6b5b6dc4d5feca8e0d93ac50516353a4ceb6edeee1', 'snapshot/mirror callback tail changed'
Path(sys.argv[2]).write_text(source[loop:tail], encoding='utf-8')
