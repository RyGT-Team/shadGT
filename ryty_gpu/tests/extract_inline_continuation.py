import sys
from pathlib import Path
s=Path(sys.argv[1]).read_text()
a=s.index('static bool CanInlineContinuation(')
b=s.index('struct ParallelRuntime {',a)
c=s.index('bool NativeRenderer::ContinueSmallParallelPass(')
d=s.index('\nvoid NativeRenderer::BeginRendering(',c)
Path(sys.argv[2]).write_text(s[a:b]+s[c:d]+'\n')
