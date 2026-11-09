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
Frame-prepared B fields are sequence and guest address; timestamps are correlation anchors,
not a guaranteed one-to-one link between the sequence and present pointer.

The trace records CPU wall time and does not identify GPU waits or reflection targets by itself.
Compare to an external QPC-aligned capture and measure opt-in overhead against a matched trace-off
Release run.

