import sys
from pathlib import Path
s=Path(sys.argv[1]).read_text();out=Path(sys.argv[2])
a=s.index("    const auto same_vertex =");b=s.index("    EmitWholePass",a);(out/"vertex-same-under-test.inc").write_text(s[a:b])
a=s.index("            if (!bound_vertex || !same_vertex");b=s.index("            if(bound_pipeline!=draw.pipeline)",a);(out/"vertex-record-under-test.inc").write_text(s[a:b])
