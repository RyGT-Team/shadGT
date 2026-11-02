from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
begin = source.index("            const Region* vertex_region_hint=nullptr;")
end = source.index("                if(!owned && handle && size && ParallelDirectVertexEnabled()", begin)
Path(sys.argv[2]).write_text(source[begin:end], encoding="utf-8", newline="\n")
