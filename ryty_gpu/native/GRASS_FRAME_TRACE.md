# Grass frame-end trace

In a Debug build, capture a frame with F8 while the grass defect is visible. For a
comparison, park at the same camera position and capture before and after the
user-observed defect. The `scene-grass-pipeline` event identifies each captured
draw by ID and reports shader object use, effective primitive, vertex stage hash,
quad expansion, and the actual indexed count.

The trace records only VS `0x5a5c405d`, binding 7, stride 56. Up to 16 unique
draws are sampled, at most 28,672 bytes each (less than 512 KiB total).
`grass-vertices.csv` reports each draw and its status. A `frame_end` row has a
corresponding `grass-vertices-ID.bin`; these are frame-end GPU bytes, not
draw-time bytes, and cannot establish visibility at the draw.

At frame end the recorder checks that a current region contains the range and
still has the recorded buffer handle and offset. `no_region`,
`no_containing_region`, and `binding_changed` mean no copy was made; allocation
and file failures are also reported. Buffers that have been replaced or are not
owned by a current region are intentionally skipped.
