# Streaming upload regression coverage

Configure this directory as a standalone CMake project, build `unique_upload_batch_test` and `descriptor_update_batch_test`, then run CTest with `-R "unique_upload_batch|descriptor_update_batch"`.

The queue test checks 100,000 inputs against an independent first-occurrence oracle. The descriptor planner checks 10,240 cache-hit/miss, allocation-failure, push-set, overlapping-write and bind-order cases.

On Windows, `RytyNativeImageSelfTest` also exercises a deterministic upload race: eighteen duplicate admissions are queued, then a CPU write arrives during the first frozen snapshot. The first drain must upload once, keep the image dirty, and use the newer bytes at the next actual image use without another write notification. GPU readback verifies both snapshots, followed by partial-GPU ownership checks. The prior drain behavior fails this regression.

`RytyNativeDescriptorBatchSelfTest` checks 1,024 NVIDIA output values with cached ordinary sets and a push set in the middle. The fixture records 160 individual host-update calls versus 64 batched calls. Its shader source is `descriptor_batch_test.comp`; regenerate the embedded SPIR-V with glslangValidator using `-V` when changing the fixture.

Gameplay improvement still requires a same-scene comparison. These tests do not establish that every frame meets 16.6 ms.

GPU command-list timing is diagnostic and defaults off. Set `RYGT_GPU_TIMING=1` exactly to enable query pools and the timing reader; unset, `0`, and other values leave it off. `RytyNativeGpuTimingSelfTest` checks the environment-controlled factory and explicitly exercises both disabled and enabled timing, including its bounded query-lease tests.
