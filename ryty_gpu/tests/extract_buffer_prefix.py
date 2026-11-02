from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
out = Path(sys.argv[2])
start = source.index("    // Seal only a complete factory-built buffer prefix.")
end = source.index("    // Seal each complete image/sampler row independently.", start)
(out / "buffer-prefix-producer.inc").write_text(source[start:end], encoding="utf-8")
start = source.index("                bool buffer_prefix=false;")
end = source.index("                if(known_structure)++parallel->producer_structure_hits;", start)
(out / "buffer-prefix-recorder.inc").write_text(source[start:end], encoding="utf-8")
start = source.index("                if(descriptor_safe && (image_batch_infos || buffer_prefix || native_vi_receipt_used)) {")
end = source.index("                parallel->producer_upload_lookup_hits", start)
(out / "buffer-prefix-end.inc").write_text(source[start:end], encoding="utf-8")
loop = source.index("                std::size_t action_index=buffer_prefix?b.buffers.size():0")
loop_end = source.index("                    if(!known_structure && (write.pNext", loop)
(out / "buffer-prefix-loop.inc").write_text(source[loop:loop_end], encoding="utf-8")
assert "std::countr_zero(pending[word])" in source[loop:loop_end]
assert "Splice(" not in source[loop:start]

start = source.index("        if(descriptor_safe && b.writes.empty() && native_vi_receipt_used &&")
end = source.index("        whole_phases.Next(7);", start)
(out / "buffer-prefix-empty-final.inc").write_text(source[start:end], encoding="utf-8")

start = source.index("        const bool native_receipt_valid=trusted && from_prepared")
end = source.index("        if(vertex_requested && has_vertex_input && vertex_preflight", start)
(out / "native-vi-admission.inc").write_text(source[start:end], encoding="utf-8")
