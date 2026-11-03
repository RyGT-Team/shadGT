import sys
from pathlib import Path
s=Path(sys.argv[1]).read_text()
a=s.index("enum PassCloseReason : u32")
b=s.index("struct ParallelRuntime {",a)
Path(sys.argv[2]).write_text(s[a:b])
