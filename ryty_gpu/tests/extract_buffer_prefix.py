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
start = source.index("                if(descriptor_safe && (image_batch_infos || buffer_prefix)) {")
end = source.index("                parallel->producer_upload_lookup_hits", start)
(out / "buffer-prefix-end.inc").write_text(source[start:end], encoding="utf-8")
loop = source.index("                std::size_t action_index=buffer_prefix?b.buffers.size():0")
loop_end = source.index("                    if(!known_structure && (write.pNext", loop)
(out / "buffer-prefix-loop.inc").write_text(source[loop:loop_end], encoding="utf-8")
assert "std::countr_zero(pending[word])" in source[loop:loop_end]
assert "Splice(" not in source[loop:start]
