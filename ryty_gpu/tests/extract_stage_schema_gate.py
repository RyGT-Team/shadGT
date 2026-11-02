from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text(encoding="utf-8")
start=source.index("        const auto current_stages=pipeline->GetStages();")
end=source.index("        if(!resource_key_known) ++parallel->unknown_resource_key;",start)
Path(sys.argv[2]).write_text(source[start:end],encoding="utf-8")
