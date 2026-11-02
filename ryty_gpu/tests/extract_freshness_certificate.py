from pathlib import Path
import sys

header = Path(sys.argv[1]).read_text(encoding="utf-8")
source = Path(sys.argv[2]).read_text(encoding="utf-8")
start = header.index("    class ChangeLog {")
end = header.index("\n    ChangeLog buffer_log;", start)
klass = header[start:end].strip()
start = source.index("void NativeRenderer::ChangeLog::Note(")
end = source.index("\nNativeRenderer::PreparedDraw* NativeRenderer::NewPrepared()", start)
methods = source[start:end].replace("NativeRenderer::ChangeLog::", "ChangeLog::")
start = source.index("        if(!fast_reject && (prepared->unmap_gen!=")
end = source.index("\n        if(fast_reject)", start)
out = Path(sys.argv[3])
(out / "freshness-log-under-test.inc").write_text(klass + "\n" + methods, encoding="utf-8")
(out / "freshness-admission-under-test.inc").write_text(source[start:end], encoding="utf-8")
