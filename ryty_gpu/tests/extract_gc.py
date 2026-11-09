from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
begin = source.index('void TextureCache::GarbageCollectImages() {')
end = source.index('\nvoid TextureCache::GarbageCollectSamplers()', begin)
Path(sys.argv[2]).write_text(source[begin:end], encoding='utf-8', newline='\n')
