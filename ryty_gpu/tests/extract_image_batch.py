from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
out = Path(sys.argv[2])
snapshot = Path(sys.argv[3]).read_text(encoding="utf-8")
start = snapshot.index("    template<class T> static bool Inside(")
end = snapshot.index("\npublic:", start)
(out / "image-batch-inside.inc").write_text(snapshot[start:end], encoding="utf-8")
start = source.index("    // Seal each complete image/sampler row independently.")
end = source.index("    // An off/on transition invalidates all tokens", start)
(out / "image-batch-producer.inc").write_text(source[start:end], encoding="utf-8")
start = source.index("                const bool image_batch=known_structure")
end = source.index("                if(!image_batch)", start)
(out / "image-batch-start.inc").write_text(source[start:end], encoding="utf-8")
start = source.index("                if(descriptor_safe && (image_batch_infos || buffer_prefix)) {")
end = source.index("                parallel->producer_upload_lookup_hits", start)
(out / "image-batch-end.inc").write_text(source[start:end], encoding="utf-8")
skip_start = source.index("                    if(image_batch && action_index<=128")
skip_end = source.index("                    if(!known_structure && (write.pBufferInfo", skip_start)
(out / "image-batch-skip.inc").write_text(source[skip_start:skip_end], encoding="utf-8")
assert skip_end < source.index("Native::Parallel::AbsentSampledImage<Shader::Info>(", skip_end) < start
assert start < source.index("                    if(!b.groups_ready)b.PrepareWriteGroups();", start)
worker_start = source.index("    const bool worker_identity = worker_result")
worker_end = source.index(";", worker_start)
worker = source[worker_start:worker_end]
for field in ("prepared_identity==prepared", "pipeline_identity==pipeline", "pipeline_layout==pipeline->GetLayout()", "writes_data==b.writes.data()", "writes_size==b.writes.size()", "images_data==b.images.data()", "images_size==b.images.size()"):
    assert field in worker
assert "const bool known_structure=worker_identity && worker_result->descriptor_structure" in source
textures = source[source.index("bool NativeRenderer::PrepareTextures("):]
factory = textures.index("const auto sampler_handle=SamplerFor(")
epoch = textures.index("const auto origin_epoch=")
invalid = textures.index("p.sampler_origins_valid=false;", factory)
record = textures.index("p.sampler_origins.push_back({b.images.size(),sampler_handle});", factory)
publish = textures.index("b.images.push_back({sampler_handle, VK_NULL_HANDLE", factory)
assert epoch < factory < invalid < record < publish
capture = textures[factory:record]
assert "!sampler_handle" in capture and "p.sampler_origins.size()==p.sampler_origins.capacity()" in capture
assert "p.sampler_origins_epoch!=origin_epoch" in capture
assert "sampler_cache_epoch.load(std::memory_order_acquire)!=origin_epoch" in capture
start = textures.index("        const auto origin_epoch=")
end = textures.index("        b.write_sets.push_back", start)
(out / "image-batch-factory.inc").write_text(textures[start:end], encoding="utf-8")
