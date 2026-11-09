import sys
import re
from pathlib import Path
s=Path(sys.argv[1]).read_text();out=Path(sys.argv[2])
a=s.index("    const auto same_vertex =");b=s.index("    EmitWholePass",a);(out/"vertex-same-under-test.inc").write_text(s[a:b])
a=s.index("            if (!bound_vertex || !same_vertex");b=s.index("            if(bound_pipeline!=draw.pipeline)",a)
# The replay fixture extracts only the command body, without the pass-local debug probe.
body=re.sub(r"(?m)^#if !defined\(NDEBUG\)\n.*?^#endif\n", "", s[a:b], flags=re.S)
(out/"vertex-record-under-test.inc").write_text(body)
