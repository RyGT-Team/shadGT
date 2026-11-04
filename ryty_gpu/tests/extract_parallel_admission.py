from pathlib import Path
import sys

native, graphics, header = [Path(p).read_text(encoding="utf-8") for p in sys.argv[1:4]]
out = Path(sys.argv[4])

def emit(name, text, begin, end):
    start = text.index(begin)
    finish = text.index(end, start)
    (out / name).write_text(text[start:finish], encoding="utf-8")

emit("admission-stage-type.inc", header, "    struct ParallelStageSummary {", "    const ParallelStageSummary&")
emit("admission-stage-build.inc", graphics, "void GraphicsPipeline::SetStageInfos(", "GraphicsPipeline::GraphicsPipeline(")
assert "    SetStageInfos(infos);" in graphics[graphics.index("GraphicsPipeline::GraphicsPipeline("):]
emit("admission-stage-select.inc", native, "        bool stages_safe=true,resource_key_known=true;", "        if(!resource_key_known)")
identity_start = native.index("        worker_identity = worker_result")
identity_end = native.index(";", identity_start) + 1
(out / "admission-worker-identity.inc").write_text("    const bool " + native[identity_start:identity_end].lstrip(), encoding="utf-8")
emit("admission-vertex-build.inc", native, "        if(p->has_vertices && p->vertex_index) {", "        candidate->counts_valid=")
emit("admission-vertex-select.inc", native, "        const bool known_shape=worker_identity", "            const bool native_vertex=vertex_safe && known_shape")
