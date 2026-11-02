from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
begin = source.index("            const Region* vertex_region_hint=nullptr;")
end = source.index("                if(!owned && handle && size && ParallelDirectVertexEnabled()", begin)
Path(sys.argv[2]).write_text(source[begin:end], encoding="utf-8", newline="\n")

start = source.index("    const auto valid_native = [&](const auto& owner) {")
end = source.index("    {\n        if (Region* region = RegionOver(address, size))", start)
Path(sys.argv[3]).write_text(source[start:end], encoding="utf-8", newline="\n")

start = source.index("            const bool native_vertex=vertex_safe && known_shape")
end = source.index("            if(!native_vertex) {", start)
Path(sys.argv[4]).write_text(source[start:end], encoding="utf-8", newline="\n")
start = source.index("                if(native_receipt_valid && prepared->native_index_receipt)")
end = source.index("\n                else {", start)
Path(sys.argv[5]).write_text(source[start:end], encoding="utf-8", newline="\n")

job_start=source.index("std::optional<std::pair<vk::Buffer, u64>> NativeRenderer::JobVertexBuffer(")
job_end=source.index("bool NativeRenderer::PrepareVertices(",job_start)
job=source[job_start:job_end]
assert "if(native_receipt)*native_receipt = false;" in job
branch=job.index("    auto* direct = r.buffer_cache.GetDirectMemory();")
assert "*native_receipt" not in job[branch:]
