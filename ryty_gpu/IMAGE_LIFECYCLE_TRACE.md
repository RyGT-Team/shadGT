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
Frame-prepared B fields are sequence and guest address; timestamps are correlation anchors,
not a guaranteed one-to-one link between the sequence and present pointer.

The trace records CPU wall time and does not identify GPU waits or reflection targets by itself.
Compare to an external QPC-aligned capture and measure opt-in overhead against a matched trace-off
Release run.

