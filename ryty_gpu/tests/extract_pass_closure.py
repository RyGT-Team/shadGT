import sys
from pathlib import Path
s=Path(sys.argv[1]).read_text()
a=s.index("enum PassCloseReason : u32")
b=s.index("struct ParallelRuntime {",a)
Path(sys.argv[2]).write_text(s[a:b])

h=Path(sys.argv[3]).read_text()
a=h.index("template<class Begin, class Emit, class End>")
b=h.index("// Records one complete rendering scope",a)
Path(sys.argv[4]).write_text(h[a:b])

a=s.index("        const bool inline_safe=bool(parallel->active)")
b=s.index(";",a)+1
Path(sys.argv[5]).write_text(s[a:b])
