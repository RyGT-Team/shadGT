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
Image-create B fields are width, height, Vulkan format, usage, and packed layers/mips; E fields
are the image handle and allocation bytes. Recycle lookup E field a is 1 for a hit, 0 for a miss.
Present B fields are frame pointer, reuse flag, game-frame flag, and anti-lag frame token.
Frame-prepared B fields are sequence and guest address; timestamps are correlation anchors,
not a guaranteed one-to-one link between the sequence and present pointer.

The trace records CPU wall time and does not identify GPU waits or reflection targets by itself.
Compare to an external QPC-aligned capture and measure opt-in overhead against a matched trace-off
Release run.

