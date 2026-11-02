from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text(encoding="utf-8");out=Path(sys.argv[2])
start=source.index("    if(backing_proven) *backing_proven=bool(handle)")
end=source.index("    return {handle, offset",start)
(out/"upload-backing-proof.inc").write_text(source[start:end],encoding="utf-8")
line="    s.upload_incarnation=s.upload_incarnation && s.upload_incarnation!=UINT64_MAX ? s.upload_incarnation+1 : 0;"
assert source.count(line)==2
(out/"upload-incarnation-bump.inc").write_text(line+"\n",encoding="utf-8")
start=source.index("    p.upload_receipt_incarnation=0;",source.index("bool NativeRenderer::CommitPrepared("))
end=source.index("    trace.Next(6);",start)
(out/"upload-commit-proof.inc").write_text(source[start:end],encoding="utf-8")
