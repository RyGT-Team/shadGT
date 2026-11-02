from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text(encoding="utf-8")
start = source.index("bool DirectMemory::TryForEachPart(")
end = source.index("\nvoid DirectMemory::WarmImported", start)
Path(sys.argv[2]).write_text(source[start:end], encoding="utf-8")
