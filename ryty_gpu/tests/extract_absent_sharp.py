from pathlib import Path
import sys

native = Path(sys.argv[1]).read_text(encoding="utf-8")
rasterizer = Path(sys.argv[2]).read_text(encoding="utf-8")
out = Path(sys.argv[3])
start = native.index("                        if(action.stage_index<stages.size() && stages[action.stage_index] &&", native.index("const bool build_actions="))
end = native.index("                        // Indices are hints only;", start)
(out / "absent-producer.inc").write_text(native[start:end], encoding="utf-8")
start = native.index("const bool certified_absent=trusted && from_prepared")
end = native.index(";", start) + 1
(out / "absent-recorder.inc").write_text(native[start:end], encoding="utf-8")
entry = rasterizer.index("bool Rasterizer::InstallDrawStages(")
body = rasterizer.index("{", entry) + 1
start = rasterizer.index("    if (draw_stage_install_serial != UINT64_MAX) ++draw_stage_install_serial;", body)
assert not rasterizer[body:start].strip(), "install serial must be the first statement"
end = rasterizer.index("\n", start)
(out / "absent-install.inc").write_text(rasterizer[start:end], encoding="utf-8")
