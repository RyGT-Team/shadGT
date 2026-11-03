import sys
from pathlib import Path
s=Path(sys.argv[1]).read_text()
a=s.index("    template<bool Proven=false,class Groups>")
b=s.index("    Admission CheckCounts(",a)
c=s.index("    template<class Rows> void Stage(",b)
d=s.index("    void Flush(",c)
Path(sys.argv[2]).write_text(s[a:b]+s[c:d])
