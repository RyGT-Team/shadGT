# Image lifecycle trace (DIAG-378-GTS)

Windows RyGTS Release can collect a bounded, opt-in CPU-side trace without image readbacks.
Set `RYGT_IMAGE_LIFECYCLE_TRACE=1` before launching. Once in the desired scene, create
`logs/image-lifecycle-<PID>.start` beside the game executable. The worker consumes the marker,
captures for 25 seconds, and writes `logs/image-lifecycle-<PID>.csv`. Keep the process alive
until the CSV appears.

The header records QPC frequency and capture bounds, capacity, attempted and dropped records.
B/E rows are paired by ID; spans crossing capture boundaries or dropped records can be incomplete.
S rows represent fault/lock spans of at least 200 microseconds and put start QPC, address and size
in a/b/c. Fast fault spans are omitted. A nonzero dropped count means coverage is incomplete.
For scheduler_wait, b is the requested tick. For native_fence_wait, b is the value and c is
the semaphore handle. These spans measure host wall time and do not alone prove GPU causation.
Image-create B fields are width, height, Vulkan format, usage, and packed layers/mips; E fields
are the image handle and allocation bytes. Recycle lookup E field a is 1 for a hit, 0 for a miss.
The native path emits native_image_create and native_image_create_from with width, height,
Vulkan format, usage and packed layers/mips; E field a is the image handle. native_vma_create
records width, height, Vulkan format and usage; its E fields contain the image handle and
allocation bytes on success (or the Vulkan result on failure). native_image_init_layout,
native_image_retire and native_image_destroy use image handle a. native_view_create records
image handle, base mip, mip count, base layer and layer count in B; E field a is the view handle.
native_view_retire and native_view_destroy use view handle a. Retire and actual destruction can
occur on different threads and at different times.
Presenter frame allocation and destruction are reported as presenter_image_create/destroy and
presenter_view_create/destroy; creation uses the same image fields with frame ID in e.
image_memory_requirements measures uncached small-image memory requirement queries, including
temporary image creation on the fallback path; E fields contain footprint and dedicated flag.
These events do not cover all presenter auxiliary views, swapchain images, or ImGui images.
Present B fields are frame pointer, reuse flag, game-frame flag, and anti-lag frame token.
Upload pool spans use upload_take B fields a=requested bytes and b=known completed tick;
its E fields are a=1 for pool reuse or 0 for a new allocation, and b=selected capacity.
The span starts after the existing optional timeline refresh, so it excludes refresh time.
upload_free_before_reclaim and upload_free_after_reclaim B fields are parent span ID,
free count, free bytes, fitting count, and smallest fitting capacity. Each is followed by
upload_retiring with parent ID, retiring count, retiring bytes, oldest retire tick (zero
if empty), and known completed tick. These snapshots describe cached free and retiring
chunks, not all active chunks. upload_create_buffer B fields are parent ID, requested
bytes and planned capacity; E fields are success flag and actual buffer capacity. Each
allocation attempt, including recovery attempts, receives a separate span.
Upload-only mode also includes these pool events. Successful upload-memory buffer allocations
emit upload_allocation_info (E: requested bytes, allocation bytes), upload_block_info (B:
parent ID, buffer handle, containing VMA block bytes, memory type index), and upload_heap_info
(B: parent ID, heap index, heap flags, heap size, native-upload name flag; E: memory property
flags, dedicated allocation flag). This covers every upload-memory buffer, with name flag 1
only for the native upload pool. Associate a pool creation by same-thread nesting and time;
there is no cross-scope ID. A containing block may have existed already: its size does not
establish a new device allocation or its cause. Queries use VMA cached information only and
run only during active capture. Monitor drops and observer cost in upload-only captures.
Metadata also appears in unrestricted full capture. The enclosing create-buffer span includes
metadata queries and event emission, so its duration includes diagnostic overhead.
Upload events can be frequent and add pool-walk overhead while capture is active. Check
the trace drop count and capture-window coverage before interpreting missing events.
The retiring totals do not identify which in-flight chunks fit, or independently show
whether a completed chunk was evicted by the free-pool budget.
Texture source selection and CPU staging spans are recorded only for guest sizes of at
least 4 MiB. texture_source_select B fields are guest address, size, packed width/height,
tiled flag and Vulkan pixel format. texture_cpu_source B fields are selection ID, guest
address, size and reason (1=CPU staging policy, 2=import failure). Phase spans identify
their parent ID and record allocation, mapping, readable-prefix, zero-tail, guarded-copy,
sparse fallback, wrap and local staging. Guarded-copy E field a indicates success; mapping
and readable-prefix E field a contain the resulting byte count. texture_policy_readable
and texture_ensure_imported E field a report their boolean result. Selection includes all
work until its return, and can enclose the CPU stages. Events may cross capture boundaries
or be dropped when the shared ring fills; sizes below the threshold are not represented.
Selection does not report a final source kind: a missing CPU child does not identify which
other route was selected, especially with capture-boundary clipping or dropped events.
For CPU-origin entries of at least 4 MiB, texture_entry_create/evict B fields are entry
pointer, guest address, size and (eviction only) total uploads; E field a is image handle.
texture_entry_upload B fields are entry pointer, guest address, size and consumed force
flag; E fields are prior total uploads and write version. texture_source_entry B fields
are selection ID, entry pointer, prior uploads, write version and foreign clock; E field a
is image handle. Counters are observational atomic snapshots, not a synchronized state.
texture_entry_stale B fields are entry pointer, reason (0 CPU, 1 unmap, 2 GPU), origin,
address and size; E fields are exact and flags (bit 0 previously dirty, bit 1 force set).
Origin 1 is Invalidate, 2 new mapping, 3 CpuWrite, 4 GpuWrote, 5 layer check, and 0 is
another caller without supplied range. CpuWrite exact is a byte extent from its address,
zero meaning unknown; GpuWrote exact is a boolean. Entry pointers and image handles can
be reused: use creation/eviction chronology, and treat identities as ambiguous when the
capture starts mid-lifetime, ends early or drops events. These events do not prove bytes
changed. Repeated stale events may be frequent and consume the shared bounded ring.
Stale event timing follows the dirty-state exchange and concurrent upload/stale events
can appear reordered; the observed force flag can race its consumption. Do not infer a
total causal order from event timestamps or the force snapshot alone.
origin_runtime_invalidate B fields are the address and size passed to the exported runtime
invalidation callback. origin_cpu_stack B fields are the registered stack address and size.
origin_write_fault B fields are fault address, reported size and decoded exact byte size
(zero when unknown); E fields are instruction pointer and handler result. These scopes
describe entry paths, not proof that bytes were written or a particular texture changed.
Correlate nested events by thread and time; clipping and dropped events can break pairing.
Fault markers can be numerous and may fill the bounded capture early.
Frame-prepared B fields are sequence and guest address; timestamps are correlation anchors,
not a guaranteed one-to-one link between the sequence and present pointer.

The trace records CPU wall time and does not identify GPU waits or reflection targets by itself.
Compare to an external QPC-aligned capture and measure opt-in overhead against a matched trace-off
Release run.

Set `RYGT_IMAGE_TRACE_UPLOAD_ONLY=1` with the main trace opt-in for workload summaries,
presentation anchors and existing image lifecycle/source/CPU staging events for requests
of at least 4 MiB. This mode takes precedence over workload-only and presentation-only;
import-only takes precedence over it. The CSV header reports `upload_only`. Other kinds and
slow-span clocks are suppressed. Existing entry create/evict/stale/upload events cover only
CPU-origin entries; source-result events cover all qualifying upload requests.
texture_source_result B fields are entry pointer, guest address, requested size, source kind,
and buffer-present flag; E fields are CPU-origin and GPU-backing-written flags. Kind 1 is
region, 2 direct import, 3 gather or local copy of an import, and 4 CPU staging (possibly
followed by a local copy). Kind 0/no buffer enters registry fallback, not proof of success.
texture_registry_fallback B fields are entry pointer, guest address and requested size; E
field a is 0 for allocation/mapping failure or 1 after copy setup, and b is readable bytes.
This does not prove copied bytes, GPU completion, or image semantics. Pointer reuse, missing
begin/end records and concurrent dirty events limit identity and ordering. Frequent stale
events can exhaust the bounded ring; inspect drops and capture boundaries.
The two new result event kinds are also included in unrestricted full trace mode; normal
trace-off behavior is unchanged.
For a content study only, set `RYGT_IMAGE_TRACE_CONTENT_PROBE=1` with active upload-only
capture. Eligible CPU-origin staging requests from 4 to 64 MiB with a fully readable range
are copied once from guest memory into a normal-memory scratch, hashed there with XXH3,
then copied into the existing upload buffer. The per-thread scratch retains up to 64 MiB
until thread exit; nested use or allocation failure falls back to the usual direct copy.
Failed guarded copies retain the usual sparse fallback and produce no valid fingerprint.
texture_content_probe B fields are selection ID, entry pointer, guest address, requested
size and hash; E fields are image handle and flags (bit 0 valid, bit 1 snapshot forwarded,
bit 2 unavailable, bit 3 partially readable). Hash/forward spans measure the added work.
Hashes describe the bytes forwarded from that snapshot, not an atomic guest-memory view;
guest writes may race the snapshot. Comparisons must exclude invalid/partial results,
entry or mapping reuse, incompatible descriptors, forced or GPU-written data, missing
events and capture boundaries. Hash matches can collide and never suppress an upload.
The extra copy and hash perturb timing: this capture is not a performance benchmark.

For point-in-time alias feasibility only, set `RYGT_IMAGE_TRACE_ALIAS_PROBE=1` with active
upload-only capture. At most 32 policy-selected CPU staging requests (4–64 MiB) per process
are attempted; this is a bounded sample, not complete or necessarily representative. Each
range is rounded out to 16 KiB pages; rounded span can exceed 64 MiB by less than 32 KiB.
The probe calls the existing runtime alias export and immediately uses the same Windows
UnmapViewOfFile2 operation as the current runtime to release the returned view, after
checking its allocation base. It performs no Vulkan import or direct-memory cache publication.
texture_alias_probe B fields are
selection ID, entry pointer, guest address, requested size and image handle; E fields are
status and one-based budget ordinal. Status 1=invalid range, 2=missing export, 4=refused,
5=successfully mapped and unmapped, 6=unmap failed; disabled and exhausted calls emit no
event. texture_alias_probe_detail B fields are parent event ID, rounded base, rounded span,
pre-call QPC and post-call QPC; E fields are the alias pointer's low 16 bits and cleanup error.
Those bits do
not establish alignment requirements above 64 KiB. A successful probe does not prove later
mapping identity, GPU ownership, host coherence, Vulkan acceptance or correct ordering. An
unmap or allocation-base query failure is reported and disables further probes; the one
unreleased alias can remain until process exit. A nonblocking single-in-flight guard skips
concurrent attempts, so capture coverage is not complete. This cleanup depends on the current
runtime MapAlias implementation creating an OS view without separate alias bookkeeping.
Captures may clip/drop records and
the extra mapping work perturbs timing; use a separate run from the content probe.

Set `RYGT_IMAGE_TRACE_WORKLOAD_ONLY=1` with the main trace opt-in for bounded workload
summaries and `present`/`frame_prepared` anchors only. It suppresses unrelated event kinds
and slow-span clocks; import-only takes precedence if both modes are set. The CSV header
reports `workload_only`. A `workload_summary` B row at NativeRenderer's FindDisplay frame
boundary contains frame number, cumulative frontend draws, cumulative serial native Draw
attempts, cumulative serial native direct/indirect dispatch attempts, and cumulative image
upload-source requested bytes. E field a contains cumulative requested HashGuestBytes bytes.
The frontend counter runs outside capture; the other counters run only while capture is open.
Compare differences between successive complete summaries, not absolute values. Image upload
requests include GPU region/import/gather and unsuccessful requests, not just CPU copies;
hash bytes are requested sizes from all HashGuestBytes callers and threads, not guaranteed
completed reads. Draws and dispatches are attempts, not necessarily submitted commands, and
parallel paths are not fully represented. Individual relaxed atomic loads can span concurrent
updates; frame bins do not guarantee correspondence to one presented frame. Capture start/end,
missing summaries and dropped records limit comparisons; tracing can add observer overhead.

Set `RYGT_IMAGE_TRACE_IMPORT_ONLY=1` with the main opt-in for a separate direct-import
capture; it takes precedence over presentation-only and suppresses other event kinds and
slow-span clocks. The CSV header records `import_only`. Roles in direct_import_operation B
are 1=WarmImported, 2=EnsureImported, 3=FindBuffer, followed by requested address, size,
mutex acquisition-call ticks and acquire-to-before-unlock ticks. EnsureImported and
FindBuffer can execute on either producer or recorder threads; use TID and external profile
for attribution. Operation E field a is success (WarmImported: lock acquired), and b is
the DirectMemory owner identity. Compare intervals for the same owner; owner pointers can
be reused across lifetimes, and timestamp overlap alone does not identify the lock holder.
direct_import_timing B has parent ID, wait-start, acquired, before-unlock and post-unlock
QPC; the latter is when deferred emission begins. direct_import_detail B has parent ID,
import attempts, successes, successfully imported bytes and total ImportView ticks; E has
maximum ImportView ticks and accumulated WarmImported try-lock misses since the previous
emitted detail (across threads). direct_import_driver B has parent ID and total alias-map,
host-pointer-properties, Vulkan allocation, and buffer-create/bind wall ticks. These are
CPU wall durations, not GPU execution time. Per-operation summaries are emitted only when
an import was attempted or lock acquisition/hold reached 200 microseconds. Fast calls,
coverage hits and busy attempts can be omitted, and the last busy aggregate may remain
unreported; do not derive complete rates from emitted records. Records are appended after
the direct-memory mutex unlock. Capture boundary crossings and ring drops can omit data;
active timing adds overhead and lock acquisition duration includes scheduling delays.
Overlapping producer and consumer intervals do not identify a lock holder by themselves.
Each emitted operation uses up to eight ring events, so check drop counts and coverage.
Nested measured calls suppress their own output to avoid emission while an outer measured
lock may be held; counts and driver totals can therefore omit nested work. Before-unlock
and post-unlock stamps bracket the unlock and any intervening scheduling, rather than
measuring the unlock instruction itself. Prefix and end-of-capture operations may be absent.

Speculative texture imports from producer-side PrepareTextures are disabled by default.
Set `RYGT_NATIVE_PREPARE_TEXTURE_WARM=1` to enable them for a controlled comparison.
Other draw preparation and recorder-side import and upload paths are unchanged. Set the
option before process startup; it is sampled on first use.

Set `RYGT_IMAGE_TRACE_PRESENT_ONLY=1` before process startup alongside the main trace opt-in
to retain only `present`, `frame_prepared`, and the explicitly listed `presentation_*` stages:
prepare_frame, free_queue, frame_fence, swapchain_acquire, queue_present, wait_request,
wait_flip_rate, and tick_sleep. The CSV header reports `present_only`. Slow fault/wait spans
and image/texture events are omitted. With the option absent, the original full trace remains.
PrepareFrame E field a identifies the returned frame; queue and fence events identify the
frame where available. The wait labels measure time in the existing calls, including lock
acquisition where applicable; they do not establish that a GPU or frame limiter caused a delay.
Free-queue timing includes mutex acquisition and queue removal. Acquire E field b is not a
valid new image index when the call fails; queue-present includes the existing DLSS-G markers.
Condition-variable waits can wake early or spuriously and do not prove deliberate pacing.
Presentation-only mode also retains upload_take, upload_free_before_reclaim,
upload_free_after_reclaim, upload_retiring, upload_create_buffer, and prepared_summary_0..3.
It also retains thresholded slow events producer_job_walk, producer_pipeline_lookup, producer_pipeline_lock,
producer_draw_prepare, fault_early_fence, fault_guard and fault_read_handler. S fields a,
b and c contain start QPC and two context values. Producer context is packet pointer, or
job pointer and walk endpoint for producer_job_walk. Fence and guard context is fault page
and a write flag (1 write, 0 read); read-handler context is fault page and reported size.
The read-handler size is the handler's capped span, and can include GPU-thread reads;
the early-fence and guard scopes apply only to non-GPU threads. Packet pointers can be reused.
These events include only calls lasting at least 200 microseconds; nested spans and shared
thread/QPC timing support correlation, not proof of causation. Active tracing adds overhead,
especially on high-frequency producer and fault paths; omitted fast spans are not zero work.
producer_pipeline_lock measures only the producer shared cache-lock acquisition call;
it includes scheduling delay. Recording a slow span adds a small cost while the lock is
held, and the event does not identify its holder.
unmap_memory B/E spans record each renderer unmap's address and size. Thresholded S spans
unmap_native, unmap_direct, unmap_buffer, unmap_gpu_sync, unmap_texture,
unmap_sparse_release and unmap_sparse_finish report start QPC, address and size. The GPU
sync span is present only for the synchronous command path; direct execution has no such
span, and includes command queueing plus execution. Texture and sparse work may run on
another thread; correlate by range and time. The full renderer span excludes the earlier
exported BeginUnmap gate and the runtime OS unmap. The sparse-finish span surrounds
only the existing scheduler.Finish in arena release. Spans can nest or overlap and their
durations must not be summed; calls below 200 microseconds are absent. Full unmap spans can
be clipped or dropped, so counts and byte totals require checking capture coverage and drops.
The summary covers the serial Draw path only; parallel prepared paths are unsupported.
Each summary E field a is the frame counter. B fields a..e are, in order:
* summary_0: missing, accepted, pipeline reject, stage reject, early unmap reject.
* summary_1: image reject, early buffer reject, late unmap reject, late buffer reject,
  post-vertex unmap reject.
* summary_2: post-vertex buffer reject, region-plan rebuild, incomplete, prepared vertex
  fallback, non-flat copied bytes.
* summary_3: flat memo hit bytes, flat copied miss bytes, materialized bytes associated
  with nonaccepted outcomes, and not-eligible draws; e is unused.
These bytes count the CommitPrepared path only. Nonaccepted bytes are not necessarily
wasted: recorded work or memo entries may still reference them. Accepted means bindings
survived these checks; a later draw return can still prevent submission. Including
upload-pool spans raises ring event volume and possible drops. Publish and frame drains
use separate relaxed atomic fields, so one draw can span bins; aggregate over longer
windows instead of interpreting individual-frame ratios. Capture-end counts may remain
unflushed, and dropped ring events leave incomplete bins. Timing remains diagnostic.
As with other spans, capture boundaries or a full ring can leave incomplete pairs.

