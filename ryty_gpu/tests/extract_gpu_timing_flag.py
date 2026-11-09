from pathlib import Path
import sys
s=Path(sys.argv[1]).read_text(encoding='utf-8')
a=s.index('static bool GpuTimingRequested(')
b=s.index('\n}\n',a)+2
Path(sys.argv[2]).write_text(s[a:b]+'\n')
