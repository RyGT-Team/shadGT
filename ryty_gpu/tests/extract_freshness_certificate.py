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

start = source.index("    const u64 recheck_from = buffer_log.Seq();")
split = source.index("    if (!buffer_log.Untouched(p.buffer_seq, p.buffer_ranges))", start)
end = source.index("    // The ordered part, as BindBuffers and BindTextures do it.", split)
(out / "freshness-first-under-test.inc").write_text(source[start:split] + "    before_first();\n" + source[split:end], encoding="utf-8")
start = source.index("    if (p.unmap_gen != unmap_generation.load(std::memory_order_acquire)) return reject(2);", end)
end = source.index("    // The viewport transform", start)
(out / "freshness-recheck-under-test.inc").write_text(source[start:end], encoding="utf-8")
