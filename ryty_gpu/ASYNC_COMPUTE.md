# Opt-in private-output GPU compute

Set `RYGT_NATIVE_ASYNC_CS=1` before starting the game to request the async path. Unset it or set it to `0` for the existing ordered path. The switch is read at startup in both Release and Debug builds. It does not clear shader caches or change CPU recorder-worker settings.

The device reserves a spare queue from the graphics-and-compute family, after any frame-generation queues. If no suitable spare queue is available, the renderer keeps the existing path. Intel's wrapped-command-buffer path also stays ordered in this candidate.

## Current dispatch coverage

This candidate offloads the already validated, completely bounded `0x8aa6e00c` fill operation. Its existing checks require captured argument bytes, matching flattened count, the expected workgroup shape and dispatch coverage, alignment, and a matching wholly covered native region. The shader hash alone does not admit work.

A small native compute kernel takes three captured CPU constants and writes a private GPU buffer. It therefore needs no graphics-producer data and can overlap graphics that was already submitted. Graphics copies its result to the original destination at the fill's original ordered position; that submission waits on the compute timeline. Existing write tracking and barriers then govern later consumers. No per-dispatch CPU finish/wait is introduced.

Other direct and indirect compute dispatches retain their existing path. Arbitrary guest compute, unknown direct-addressed aliases, image dependencies and shared GDS are not enabled by this switch.

## Lifetimes and bounds

- Compute has its own submission mutex, timeline and command-list retirement; independently completing queues never advance the same retirement timeline.
- Submitted compute command buffers are primary, from a pool in the same queue family.
- Pending image initialization remains on the graphics path.
- Private output buffers and descriptor slots are retained until both compute and the joined graphics submission complete.
- Up to eight slots, 16 MiB per dispatch and 64 MiB of active private output are admitted. Allocation, size or slot rejection uses the existing fill implementation. Deferred allocator retirement can temporarily retain older allocations beyond the active-slot budget.
- Destination copyback is restricted to a wholly covered owned native region, using its normal graphics retirement path. Imported guest backing is not used as an async destination.
- Shutdown closes outstanding graphics copybacks before destroying the async context. Its waits are teardown-only.

## Diagnostics and validation

`RytyNativeAsyncFillSelfTest` is Debug-only and exercises the actual RHI queue and kernel: gated compute, independent graphics progress, bounds/capacity, alternating-pattern tail readback, destination-backing retention through a join, and slot reuse. It does not replace game-level unmap/reload and visual checks or establish an FPS improvement.

The async dependency trace's preroll and submission-ID diagnostics remain Debug-only. Performance comparisons must use normal-play frame captures, preserve scene/camera/settings, and keep recorder workers unchanged. Actual coverage is identified by `ASYNC-CS` startup/first-submission logs; requesting the switch alone is not evidence that a scene dispatched eligible work.
